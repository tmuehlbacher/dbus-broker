/*
 * D-Bus Match Rules
 */

#include <c-list.h>
#include <c-macro.h>
#include <c-rbtree.h>
#include <c-string.h>
#include "dbus/protocol.h"
#include "match.h"
#include "util/error.h"

static int match_rules_compare(CRBTree *tree, void *k, CRBNode *rb) {
        MatchRule *rule = c_container_of(rb, MatchRule, owner_node);
        MatchRuleKeys *key1 = k, *key2 = &rule->keys;
        int r;

        if ((r = c_string_compare(key1->sender, key2->sender)) ||
            (r = c_string_compare(key1->filter.destination, key2->filter.destination)) ||
            (r = c_string_compare(key1->filter.interface, key2->filter.interface)) ||
            (r = c_string_compare(key1->filter.member, key2->filter.member)) ||
            (r = c_string_compare(key1->filter.path, key2->filter.path)) ||
            (r = c_string_compare(key1->path_namespace, key2->path_namespace)) ||
            (r = c_string_compare(key1->arg0namespace, key2->arg0namespace)))
                return r;

        if (key1->filter.type > key2->filter.type)
                return 1;
        if (key1->filter.type < key2->filter.type)
                return -1;

        if (key1->eavesdrop > key2->eavesdrop)
                return 1;
        if (key1->eavesdrop < key2->eavesdrop)
                return -1;

        for (unsigned int i = 0; i < C_ARRAY_SIZE(key1->filter.args); i ++) {
                if ((r = c_string_compare(key1->filter.args[i], key2->filter.args[i])))
                        return r;

                if ((r = c_string_compare(key1->filter.argpaths[i], key2->filter.argpaths[i])))
                        return r;
        }

        return 0;
}

static bool match_string_prefix(const char *string, const char *prefix, char delimiter) {
        char *tail;

        tail = c_string_prefix(string, prefix);
        if (!tail)
                return false;

        if (*tail != '\0' && *tail != delimiter)
                return false;

        return true;
}

static bool match_rule_keys_match_filter(MatchRuleKeys *keys, MatchFilter *filter) {
        if (keys->filter.type && keys->filter.type != filter->type)
                return false;

        if (!keys->eavesdrop && filter->destination)
                return false;

        if (keys->filter.destination && !c_string_equal(keys->filter.destination, filter->destination))
                return false;

        if (keys->filter.interface && !c_string_equal(keys->filter.interface, filter->interface))
                return false;

        if (keys->filter.member && !c_string_equal(keys->filter.member, filter->member))
                return false;

        if (keys->filter.path && !c_string_equal(keys->filter.path, filter->path))
                return false;

        if (keys->path_namespace && !match_string_prefix(filter->path, keys->path_namespace, '/'))
                return false;

        /* XXX: verify that arg0 is a (potentially single-label) bus name */
        if (keys->arg0namespace && !match_string_prefix(filter->args[0], keys->arg0namespace, '.'))
                return false;

        for (unsigned int i = 0; i < C_ARRAY_SIZE(filter->args); i ++) {
                if (keys->filter.args[i] && !c_string_equal(keys->filter.args[i], filter->args[i]))
                        return false;

                if (keys->filter.argpaths[i]) {
                        if (!match_string_prefix(filter->argpaths[i], keys->filter.argpaths[i], '/') &&
                            !match_string_prefix(keys->filter.argpaths[i], filter->argpaths[i], '/'))
                                return false;
                }
        }

        return true;
}

static int match_rule_keys_assign(MatchRuleKeys *keys, const char *key, const char *value) {
        if (strcmp(key, "type") == 0) {
                if (strcmp(value, "signal") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_SIGNAL;
                else if (strcmp(value, "method_call") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_METHOD_CALL;
                else if (strcmp(value, "method_return") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_METHOD_RETURN;
                else if (strcmp(value, "error") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_ERROR;
                else
                        return MATCH_E_INVALID;
        } else if (strcmp(key, "sender") == 0) {
                keys->sender = value;
        } else if (strcmp(key, "destination") == 0) {
                keys->filter.destination = value;
        } else if (strcmp(key, "interface") == 0) {
                keys->filter.interface = value;
        } else if (strcmp(key, "member") == 0) {
                keys->filter.member = value;
        } else if (strcmp(key, "path") == 0) {
                keys->filter.path = value;
        } else if (strcmp(key, "path_namespace") == 0) {
                keys->path_namespace = value;
        } else if (strcmp(key, "eavesdrop") == 0) {
                if (strcmp(value, "true") ==0)
                        keys->eavesdrop = true;
                else if (strcmp(value, "fase") == 0)
                        keys->eavesdrop = false;
                else
                        return MATCH_E_INVALID;
        } else if (strcmp(key, "arg0namespace") == 0) {
                keys->arg0namespace = value;
        } else if (strncmp(key, "arg", strlen("arg")) == 0) {
                unsigned int i = 0;

                key += strlen("arg");

                for (unsigned int j = 0; j < 2; j ++) {
                        if (*key < '0' || *key > '9')
                                break;

                        i = i * 10 + *key - '0';
                        key ++;
                }
                if (strcmp(key, "")  == 0) {
                        keys->filter.args[i] = value;
                } else if (strcmp(key, "path") == 0) {
                        keys->filter.argpaths[i] = value;
                } else
                        return MATCH_E_INVALID;
        } else {
                return MATCH_E_INVALID;
        }

        return 0;
}

/*
 * Takes a null-termianted stream of characters, removes any quoting, breaks them up at commas and returns them one character at a time.
 */
static char match_string_pop(const char **match, bool *quoted) {
        /*
         * Within single quotes (apostrophe), a backslash represents itself, and an apostrophe ends the quoted section. Outside single quotes, \'
         * (backslash, apostrophe) represents an apostrophe, and any backslash not followed by an apostrophe represents itself.
         */
        while (**match == '\'') {
                (*match) ++;
                *quoted = !*quoted;
        }

        switch (**match) {
        case '\0':
                return '\0';
        case ',':
                (*match) ++;

                if (*quoted)
                        return ',';
                else
                        return '\0';
        case '\\':
                (*match) ++;

                if (!(*quoted) && **match == '\'') {
                        (*match) ++;
                        return '\'';
                } else {
                        return '\\';
                }
        default:
                return *((*match) ++);
        }
}

int match_rule_keys_parse(MatchRuleKeys *keys, char *buffer, const char *rule_string, size_t n_rule_string) {
        const char *key = NULL, *value = NULL;
        bool quoted = false;
        char c;
        int r;

        for (unsigned int i = 0; i < n_rule_string; i ++) {
                if (!key) {
                        do {
                                /* strip leading space before a key */
                                c = match_string_pop(&rule_string, &quoted);
                        } while (c == ' ');
                        key = buffer + i;
                } else {
                        c = match_string_pop(&rule_string, &quoted);
                }

                /* strip key and value at '=' */
                if (c == '=' && !value) {
                        buffer[i] = '\0';
                        value = buffer + i + 1;
                } else {
                        buffer[i] = c;
                }

                /* reached end of key/value pair */
                if (c == '\0') {
                        /* did not finish reading key yet */
                        if (!value)
                                return MATCH_E_INVALID;

                        r = match_rule_keys_assign(keys, key, value);
                        if (r)
                                return error_trace(r);

                        key = NULL;
                        value = NULL;

                        /* reached the end of the input string */
                        if (*rule_string == '\0')
                                return 0;
                }
        }

        /* XXX: verify that no invalid combinations such as path/path_namespace occur */

        return MATCH_E_INVALID;
}

int match_rule_new(MatchRule **rulep, MatchOwner *owner, const char *rule_string) {
        _c_cleanup_(match_rule_freep) MatchRule *rule = NULL;
        CRBNode **slot, *parent;
        size_t n_rule_string;
        int r;

        n_rule_string = strlen(rule_string);

        rule = calloc(1, sizeof(*rule) + n_rule_string);
        if (!rule)
                return error_origin(-ENOMEM);

        rule->n_user_refs = 1;
        rule->owner = owner;
        rule->registry_link = (CList)C_LIST_INIT(rule->registry_link);
        rule->owner_node = (CRBNode)C_RBNODE_INIT(rule->owner_node);

        r = match_rule_keys_parse(&rule->keys, rule->buffer, rule_string, n_rule_string);
        if (r)
                return error_trace(r);

        slot = c_rbtree_find_slot(&owner->rule_tree, match_rules_compare, &rule->keys, &parent);
        if (!slot) {
                /* one already exists, take a ref on that instead and drop the one we created */
                *rulep = match_rule_user_ref(rule);
        } else {
                /* link the new rule into the rbtree */
                c_rbtree_add(&owner->rule_tree, parent, slot, &rule->owner_node);
                *rulep = rule;
                rule = NULL;
        }

        return 0;
}

MatchRule *match_rule_free(MatchRule *rule) {
        c_list_unlink(&rule->registry_link);
        c_rbtree_remove_init(&rule->owner->rule_tree, &rule->owner_node);

        free(rule);

        return NULL;
}

MatchRule *match_rule_user_ref(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(rule->n_user_refs > 0);

        ++rule->n_user_refs;

        return rule;
}

MatchRule *match_rule_user_unref(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(rule->n_user_refs > 0);

        --rule->n_user_refs;

        if (rule->n_user_refs == 0)
                match_rule_free(rule);

        return NULL;
}

void match_rule_link(MatchRule *rule, MatchRegistry *registry) {
        c_list_link_tail(&registry->rule_list, &rule->registry_link);
        rule->registry = registry;
}

int match_rule_get(MatchRule **rulep, MatchOwner *owner, const char *rule_string) {
        char buffer[strlen(rule_string)];
        MatchRuleKeys keys = {};
        MatchRule *rule;
        int r;

        r = match_rule_keys_parse(&keys, buffer, rule_string, strlen(rule_string));
        if (r)
                return error_trace(r);

        rule = c_rbtree_find_entry(&owner->rule_tree, match_rules_compare, &keys, MatchRule, owner_node);
        if (!rule)
                return MATCH_E_NOT_FOUND;

        *rulep = rule;
        return 0;
}

MatchRule *match_rule_next(MatchRegistry *registry, MatchRule *rule, MatchFilter *filter) {
        CList *link;

        if (!rule)
                link = c_list_loop_first(&registry->rule_list);
        else
                link = c_list_loop_next(&rule->registry_link);

        while (link != &registry->rule_list) {
                rule = c_list_entry(link, MatchRule, registry_link);

                if (match_rule_keys_match_filter(&rule->keys, filter))
                        return rule;

                link = c_list_loop_next(link);
        }

        return NULL;
}

void match_registry_init(MatchRegistry *registry) {
        *registry = (MatchRegistry)MATCH_REGISTRY_INIT(*registry);
}

void match_registry_deinit(MatchRegistry *registry) {
        assert(c_list_is_empty(&registry->rule_list));
}

void match_owner_init(MatchOwner *owner) {
        *owner = (MatchOwner){};
}

void match_owner_deinit(MatchOwner *owner) {
        assert(!owner->rule_tree.root);
}
