/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>

/* OVS includes */
#include "openvswitch/vlog.h"

/* OVN includes */
#include "debug.h"
#include "en-global-config.h"
#include "include/ovn/features.h"
#include "ipam.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "northd.h"


VLOG_DEFINE_THIS_MODULE(en_global_config);

/* static function declarations. */
static void northd_enable_all_features(struct ed_type_global_config *);
static void build_chassis_features(const struct sbrec_chassis_table *,
                                   struct chassis_features *);
static bool chassis_features_changed(const struct chassis_features *,
                                     const struct chassis_features *);
static bool config_out_of_sync(const struct smap *config,
                               const struct smap *saved_config,
                               const char *key, bool must_be_present);
static bool check_nb_options_out_of_sync(const struct nbrec_nb_global *,
                                         struct ed_type_global_config *);
static void update_sb_config_options_to_sbrec(struct ed_type_global_config *,
                                              const struct sbrec_sb_global *);

void *
en_global_config_init(struct engine_node *node OVS_UNUSED,
                      struct engine_arg *args OVS_UNUSED)
{
    struct ed_type_global_config *data = xzalloc(sizeof *data);
    smap_init(&data->nb_options);
    smap_init(&data->sb_options);
    northd_enable_all_features(data);
    return data;
}

void
en_global_config_run(struct engine_node *node , void *data)
{
    const struct engine_context *eng_ctx = engine_get_context();
    if (!eng_ctx->ovnnb_idl_txn || !eng_ctx->ovnsb_idl_txn) {
        return;
    }

    const struct nbrec_nb_global_table *nb_global_table =
        EN_OVSDB_GET(engine_get_input("NB_nb_global", node));
    const struct sbrec_sb_global_table *sb_global_table =
        EN_OVSDB_GET(engine_get_input("SB_sb_global", node));
    const struct sbrec_chassis_table *sbrec_chassis_table =
        EN_OVSDB_GET(engine_get_input("SB_chassis", node));

    struct ed_type_global_config *config_data = data;

    /* Sync ipsec configuration.
     * Copy nb_cfg from northbound to southbound database.
     * Also set up to update sb_cfg once our southbound transaction commits. */
    const struct nbrec_nb_global *nb =
        nbrec_nb_global_table_first(nb_global_table);
    if (!nb) {
        nb = nbrec_nb_global_insert(eng_ctx->ovnnb_idl_txn);
    }

    const char *mac_addr_prefix = set_mac_prefix(smap_get(&nb->options,
                                                          "mac_prefix"));

    const char *monitor_mac = smap_get(&nb->options, "svc_monitor_mac");
    if (monitor_mac) {
        if (eth_addr_from_string(monitor_mac,
                                 &config_data->svc_monitor_mac_ea)) {
            snprintf(config_data->svc_monitor_mac,
                     sizeof config_data->svc_monitor_mac,
                     ETH_ADDR_FMT,
                     ETH_ADDR_ARGS(config_data->svc_monitor_mac_ea));
        } else {
            monitor_mac = NULL;
        }
    }

    struct smap *options = &config_data->nb_options;
    smap_destroy(options);
    smap_clone(options, &nb->options);

    smap_replace(options, "mac_prefix", mac_addr_prefix);

    if (!monitor_mac) {
        eth_addr_random(&config_data->svc_monitor_mac_ea);
        snprintf(config_data->svc_monitor_mac,
                 sizeof config_data->svc_monitor_mac, ETH_ADDR_FMT,
                 ETH_ADDR_ARGS(config_data->svc_monitor_mac_ea));
        smap_replace(options, "svc_monitor_mac",
                     config_data->svc_monitor_mac);
    }

    char *max_tunid = xasprintf("%d",
        get_ovn_max_dp_key_local(sbrec_chassis_table));
    smap_replace(options, "max_tunid", max_tunid);
    free(max_tunid);

    char *ovn_internal_version = ovn_get_internal_version();
    if (strcmp(ovn_internal_version,
                smap_get_def(options, "northd_internal_version", ""))) {
        smap_replace(options, "northd_internal_version",
                     ovn_internal_version);
        config_data->ovn_internal_version_changed = true;
    } else {
        config_data->ovn_internal_version_changed = false;
    }

    free(ovn_internal_version);

    if (!smap_equal(&nb->options, options)) {
        nbrec_nb_global_verify_options(nb);
        nbrec_nb_global_set_options(nb, options);
    }

    if (smap_get_bool(&nb->options, "ignore_chassis_features", false)) {
        northd_enable_all_features(config_data);
    } else {
        build_chassis_features(sbrec_chassis_table, &config_data->features);
    }

    init_debug_config(nb);

    const struct sbrec_sb_global *sb =
        sbrec_sb_global_table_first(sb_global_table);
    if (!sb) {
        sb = sbrec_sb_global_insert(eng_ctx->ovnsb_idl_txn);
    }
    if (nb->ipsec != sb->ipsec) {
        sbrec_sb_global_set_ipsec(sb, nb->ipsec);
    }

    /* Set up SB_Global (depends on chassis features). */
    update_sb_config_options_to_sbrec(config_data, sb);

    engine_set_node_state(node, EN_UPDATED);
}

void en_global_config_cleanup(void *data OVS_UNUSED)
{
    struct ed_type_global_config *config_data = data;
    smap_destroy(&config_data->nb_options);
    smap_destroy(&config_data->sb_options);
    destroy_debug_config();
}

void
en_global_config_clear_tracked_data(void *data)
{
    struct ed_type_global_config *config_data = data;
    config_data->tracked = false;
    config_data->tracked_data.nb_options_changed = false;
    config_data->tracked_data.chassis_features_changed = false;
}

bool
global_config_nb_global_handler(struct engine_node *node, void *data)
{
    const struct nbrec_nb_global_table *nb_global_table =
        EN_OVSDB_GET(engine_get_input("NB_nb_global", node));
    const struct sbrec_sb_global_table *sb_global_table =
        EN_OVSDB_GET(engine_get_input("SB_sb_global", node));

    const struct nbrec_nb_global *nb =
        nbrec_nb_global_table_first(nb_global_table);
    if (!nb) {
        return false;
    }

    const struct sbrec_sb_global *sb =
        sbrec_sb_global_table_first(sb_global_table);
    if (!sb) {
        return false;
    }

    /* We are only interested in ipsec and options column. */
    if (!nbrec_nb_global_is_updated(nb, NBREC_NB_GLOBAL_COL_IPSEC)
        && !nbrec_nb_global_is_updated(nb, NBREC_NB_GLOBAL_COL_OPTIONS)) {
        return true;
    }

    if (nb->ipsec != sb->ipsec) {
        sbrec_sb_global_set_ipsec(sb, nb->ipsec);
    }

    struct ed_type_global_config *config_data = data;
    config_data->tracked = true;

    if (smap_equal(&nb->options, &config_data->nb_options)) {
        return true;
    }

    /* Return false if an option is out of sync and requires updating the
     * NB config. (Like svc_monitor_mac, max_tunid and mac_prefix). */
    /* Check if svc_monitor_mac has changed or not. */
    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "svc_monitor_mac", true)) {
        return false;
    }

    /* Check if max_tunid has changed or not. */
    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "max_tunid", true)) {
        return false;
    }

    /* Check if mac_prefix has changed or not. */
    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "mac_prefix", true)) {
        return false;
    }

    /* Check if ignore_chassis_features has changed or not. */
    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "ignore_chassis_features", false)) {
        return false;
    }

    /* Check if northd_internal_version has changed or not. */
    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "northd_internal_version", false)) {
        return false;
    }

    if (check_nb_options_out_of_sync(nb, config_data)) {
        config_data->tracked_data.nb_options_changed = true;
    }

    smap_destroy(&config_data->nb_options);
    smap_clone(&config_data->nb_options, &nb->options);

    update_sb_config_options_to_sbrec(config_data, sb);

    engine_set_node_state(node, EN_UPDATED);
    return true;
}

bool
global_config_sb_global_handler(struct engine_node *node, void *data)
{
    const struct sbrec_sb_global_table *sb_global_table =
        EN_OVSDB_GET(engine_get_input("SB_sb_global", node));

    const struct sbrec_sb_global *sb =
        sbrec_sb_global_table_first(sb_global_table);
    if (!sb) {
        return false;
    }

    struct ed_type_global_config *config_data = data;

    if (!smap_equal(&sb->options, &config_data->sb_options)) {
        return false;
    }

    /* No need to update the engine node. */
    return true;
}

bool
global_config_sb_chassis_handler(struct engine_node *node, void *data)
{
    struct ed_type_global_config *config_data = data;

    const struct sbrec_chassis_table *sbrec_chassis_table =
        EN_OVSDB_GET(engine_get_input("SB_chassis", node));
    const struct sbrec_chassis *chassis;

    SBREC_CHASSIS_TABLE_FOR_EACH_TRACKED (chassis, sbrec_chassis_table) {
        if (sbrec_chassis_is_new(chassis)
            || sbrec_chassis_is_deleted(chassis)
            || sbrec_chassis_is_updated(chassis,
                                        SBREC_CHASSIS_COL_ENCAPS)) {
            return false;
        }

        for (size_t i = 0; i < chassis->n_encaps; i++) {
            if (sbrec_encap_row_get_seqno(chassis->encaps[i],
                                          OVSDB_IDL_CHANGE_MODIFY) > 0) {
                return false;
            }
        }
    }

    if (smap_get_bool(&config_data->nb_options, "ignore_chassis_features",
                      false)) {
        return true;
    }

    bool reevaluate_chassis_features = false;

    /* Check and evaluate chassis features. */
    SBREC_CHASSIS_TABLE_FOR_EACH_TRACKED (chassis, sbrec_chassis_table) {
        if (sbrec_chassis_is_updated(chassis,
                                        SBREC_CHASSIS_COL_OTHER_CONFIG)) {
            reevaluate_chassis_features = true;
            break;
        }
    }

    if (!reevaluate_chassis_features) {
        return true;
    }

    struct chassis_features present_features = config_data->features;

    /* Enable all features before calling build_chassis_features() as
    * build_chassis_features() only sets the feature flags to false. */
    northd_enable_all_features(config_data);
    build_chassis_features(sbrec_chassis_table, &config_data->features);

    if (chassis_features_changed(&present_features, &config_data->features)) {
        config_data->tracked_data.chassis_features_changed = true;
        config_data->tracked = true;
        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

/* generic global config handler for any engine node which has global_config
 * has an input node . */
bool
node_global_config_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    struct ed_type_global_config *global_config =
        engine_get_input_data("global_config", node);

    if (!global_config->tracked
        || global_config->tracked_data.chassis_features_changed
        || global_config->tracked_data.nb_options_changed) {
        return false;
    }

    return true;
}

/* static functions. */
static void
northd_enable_all_features(struct ed_type_global_config *data)
{
    data->features = (struct chassis_features) {
        .ct_no_masked_label = true,
        .mac_binding_timestamp = true,
        .ct_lb_related = true,
        .fdb_timestamp = true,
        .ls_dpg_column = true,
        .ct_commit_nat_v2 = true,
        .ct_commit_to_zone = true,
    };
}

static void
build_chassis_features(const struct sbrec_chassis_table *sbrec_chassis_table,
                       struct chassis_features *chassis_features)
{
    const struct sbrec_chassis *chassis;

    SBREC_CHASSIS_TABLE_FOR_EACH (chassis, sbrec_chassis_table) {
        /* Only consider local AZ chassis.  Remote ones don't install
         * flows generated by the local northd.
         */
        if (smap_get_bool(&chassis->other_config, "is-remote", false)) {
            continue;
        }

        bool ct_no_masked_label =
            smap_get_bool(&chassis->other_config,
                          OVN_FEATURE_CT_NO_MASKED_LABEL,
                          false);
        if (!ct_no_masked_label && chassis_features->ct_no_masked_label) {
            chassis_features->ct_no_masked_label = false;
        }

        bool mac_binding_timestamp =
            smap_get_bool(&chassis->other_config,
                          OVN_FEATURE_MAC_BINDING_TIMESTAMP,
                          false);
        if (!mac_binding_timestamp &&
            chassis_features->mac_binding_timestamp) {
            chassis_features->mac_binding_timestamp = false;
        }

        bool ct_lb_related =
            smap_get_bool(&chassis->other_config,
                          OVN_FEATURE_CT_LB_RELATED,
                          false);
        if (!ct_lb_related &&
            chassis_features->ct_lb_related) {
            chassis_features->ct_lb_related = false;
        }

        bool fdb_timestamp =
            smap_get_bool(&chassis->other_config,
                          OVN_FEATURE_FDB_TIMESTAMP,
                          false);
        if (!fdb_timestamp &&
            chassis_features->fdb_timestamp) {
            chassis_features->fdb_timestamp = false;
        }

        bool ls_dpg_column =
            smap_get_bool(&chassis->other_config,
                          OVN_FEATURE_LS_DPG_COLUMN,
                          false);
        if (!ls_dpg_column &&
            chassis_features->ls_dpg_column) {
            chassis_features->ls_dpg_column = false;
        }

        bool ct_commit_nat_v2 =
                smap_get_bool(&chassis->other_config,
                              OVN_FEATURE_CT_COMMIT_NAT_V2,
                              false);
        if (!ct_commit_nat_v2 &&
            chassis_features->ct_commit_nat_v2) {
            chassis_features->ct_commit_nat_v2 = false;
        }

        bool ct_commit_to_zone =
                smap_get_bool(&chassis->other_config,
                              OVN_FEATURE_CT_COMMIT_TO_ZONE,
                              false);
        if (!ct_commit_to_zone &&
            chassis_features->ct_commit_to_zone) {
            chassis_features->ct_commit_to_zone = false;
        }
    }
}

static bool
config_out_of_sync(const struct smap *config, const struct smap *saved_config,
                   const char *key, bool must_be_present)
{
    const char *value = smap_get(config, key);
    if (!value && must_be_present) {
        return true;
    }

    const char *saved_value = smap_get(saved_config, key);
    if (!saved_value && must_be_present) {
        return true;
    }

    if (!value && !saved_value) {
        return false;
    }

    if (!value || !saved_value) {
        return true;
    }

    return strcmp(value, saved_value);
}

static bool
check_nb_options_out_of_sync(const struct nbrec_nb_global *nb,
                             struct ed_type_global_config *config_data)
{
    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "mac_binding_removal_limit", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "fdb_removal_limit", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "controller_event", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "ignore_lsp_down", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "use_ct_inv_match", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "default_acl_drop", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "debug_drop_domain_id", false)) {
        init_debug_config(nb);
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "debug_drop_collector_set", false)) {
        init_debug_config(nb);
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "use_common_zone", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "install_ls_lb_from_router", false)) {
        return true;
    }

    if (config_out_of_sync(&nb->options, &config_data->nb_options,
                           "bcast_arp_req_flood", false)) {
        return true;
    }

    return false;
}

static void
update_sb_config_options_to_sbrec(struct ed_type_global_config *config_data,
                                  const struct sbrec_sb_global *sb)
{
    struct smap *options = &config_data->sb_options;

    smap_destroy(options);
    smap_clone(options, &config_data->nb_options);

    /* Inform ovn-controllers whether LB flows will use ct_mark (i.e., only
     * if all chassis support it).  If not explicitly present in the database
     * the default value to be used for this option is 'true'.
     */
    if (!config_data->features.ct_no_masked_label) {
        smap_replace(options, "lb_hairpin_use_ct_mark", "false");
    } else {
        smap_remove(options, "lb_hairpin_use_ct_mark");
    }

    /* Hackaround SB_global.options overwrite by NB_Global.options for
     * 'sbctl_probe_interval' option.
     */
    const char *sip = smap_get(&sb->options, "sbctl_probe_interval");
    if (sip) {
        smap_replace(options, "sbctl_probe_interval", sip);
    }

    /* Adds indication that northd is handling explicit output after
     * arp/nd_ns action. */
    smap_add(options, "arp_ns_explicit_output", "true");

    if (!smap_equal(&sb->options, options)) {
        sbrec_sb_global_set_options(sb, options);
    }
}

static bool
chassis_features_changed(const struct chassis_features *present,
                         const struct chassis_features *updated)
{
    if (present->ct_no_masked_label != updated->ct_no_masked_label) {
        return true;
    }

    if (present->mac_binding_timestamp != updated->mac_binding_timestamp) {
        return true;
    }

    if (present->ct_lb_related != updated->ct_lb_related) {
        return true;
    }

    if (present->fdb_timestamp != updated->fdb_timestamp) {
        return true;
    }

    if (present->ls_dpg_column != updated->ls_dpg_column) {
        return true;
    }

    return false;
}
