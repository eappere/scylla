/*
 * Copyright (C) 2021 Criteo
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "auth/rest_role_manager.hh"

#include <optional>
#include <unordered_set>
#include <vector>

#include <boost/algorithm/string/join.hpp>
#include <seastar/core/future-util.hh>
#include <seastar/core/print.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/thread.hh>

#include "auth/common.hh"
#include "auth/roles-metadata.hh"
#include "cql3/query_processor.hh"
#include "cql3/untyped_result_set.hh"
#include "db/consistency_level_type.hh"
#include "exceptions/exceptions.hh"
#include "log.hh"
#include "utils/class_registrator.hh"
#include "replica/database.hh"

namespace auth {
namespace meta {
//
// NB: role attribute managment replicated from standard_role_manager.cc
//
namespace role_attributes_table {
constexpr std::string_view name{"role_attributes", 15};

static std::string_view qualified_name() noexcept {
    static const sstring instance = format("{}.{}", AUTH_KS, name);
    return instance;
}
static std::string_view creation_query() noexcept {
    static const sstring instance = format(
            "CREATE TABLE {} ("
            "  role text,"
            "  name text,"
            "  value text,"
            "  PRIMARY KEY(role, name)"
            ")",
            qualified_name());

    return instance;
}
}
}

static logging::logger log("rest_role_manager");

static const class_registrator<
        role_manager,
        rest_role_manager,
        cql3::query_processor &,
        ::service::migration_manager &> registration("com.criteo.scylladb.auth.RestManager");

struct record final {
    sstring name;
    bool is_superuser;
    bool can_login;
    role_set member_of;
};

static db::consistency_level consistency_for_role(std::string_view role_name) noexcept {
    if (role_name == meta::DEFAULT_SUPERUSER_NAME) {
        return db::consistency_level::QUORUM;
    }

    return db::consistency_level::LOCAL_ONE;
}

static bool has_can_login(const cql3::untyped_result_set_row &row) {
    return row.has("can_login") && !(boolean_type->deserialize(row.get_blob("can_login")).is_null());
}

std::string_view rest_role_manager::qualified_java_name() const noexcept {
    return "com.criteo.scylladb.auth.RestManager";
}

const resource_set &rest_role_manager::protected_resources() const {
    static const resource_set resources({make_data_resource(meta::AUTH_KS, meta::roles_table::name)});
    return resources;
}

future<> rest_role_manager::create_metadata_tables_if_missing() const {
    return when_all_succeed(
        create_metadata_table_if_missing(
                meta::roles_table::name,
                _qp,
                meta::roles_table::creation_query(),
                _migration_manager),
        create_metadata_table_if_missing(
                meta::role_attributes_table::name,
                _qp,
                meta::role_attributes_table::creation_query(),
                _migration_manager)).discard_result();
}

future<> rest_role_manager::create_default_role_if_missing() const {
    return default_role_row_satisfies(_qp, &has_can_login).then([this](bool exists) {
        if (!exists) {
            static const sstring query = format("INSERT INTO {} ({}, is_superuser, can_login) VALUES (?, true, true)",
                                                meta::roles_table::qualified_name,
                                                meta::roles_table::role_col_name);

            return _qp.execute_internal(
                    query,
                    db::consistency_level::QUORUM,
                    internal_distributed_query_state(),
                    {meta::DEFAULT_SUPERUSER_NAME},
                    cql3::query_processor::cache_internal::no).then([](auto&&) {
                log.info("Created default superuser role '{}'.", meta::DEFAULT_SUPERUSER_NAME);
                return make_ready_future<>();
            });
        }

        return make_ready_future<>();
    }).handle_exception_type([](const exceptions::unavailable_exception& e) {
        log.warn("Skipped default role setup: some nodes were not ready; will retry");
        return make_exception_future<>(e);
    });
}

future<> rest_role_manager::start() {
    return once_among_shards([this] {
        return this->create_metadata_tables_if_missing().then([this] {
            _stopped = auth::do_after_system_ready(_as, [this] {
                return seastar::async([this] {
                    wait_for_schema_agreement(_migration_manager, _qp.db().real_database(), _as).get0();

                    create_default_role_if_missing().get0();
                });
            });
        });
    });
}

future<> rest_role_manager::stop() {
    _as.request_abort();
    return _stopped.handle_exception_type([](const sleep_aborted &) {}).handle_exception_type(
            [](const abort_requested_exception &) {});;
}

static future <std::optional<record>> find_record(cql3::query_processor &qp, std::string_view role_name) {
    static const sstring query = format("SELECT * FROM {} WHERE {} = ?",
                                        meta::roles_table::qualified_name,
                                        meta::roles_table::role_col_name);

    return qp.execute_internal(
            query,
            consistency_for_role(role_name),
            internal_distributed_query_state(),
            {sstring(role_name)},
            cql3::query_processor::cache_internal::yes).then([](::shared_ptr <cql3::untyped_result_set> results) {
        if (results->empty()) {
            return std::optional<record>();
        }

        const cql3::untyped_result_set_row &row = results->one();

        return std::make_optional(
                record{
                        row.get_as<sstring>(sstring(meta::roles_table::role_col_name)),
                        row.get_or<bool>("is_superuser", false),
                        row.get_or<bool>("can_login", false),
                        (row.has("member_of")
                            ? row.get_set<sstring>("member_of")
                            : role_set())});
    });
}

static future <record> require_record(cql3::query_processor &qp, std::string_view role_name) {
    return find_record(qp, role_name).then([role_name](std::optional <record> mr) {
        if (!mr) {
            throw nonexistant_role(role_name);
        }

        return make_ready_future<record>(*mr);
    });
}

static future<> collect_roles(cql3::query_processor &qp, std::string_view grantee_name, role_set &roles) {
    return require_record(qp, grantee_name).then([&qp, &roles](record r) {
        return do_with(std::move(r.member_of), [&qp, &roles](const role_set &memberships) {
            return do_for_each(memberships.begin(), memberships.end(), [&qp, &roles](const sstring &role_name) {
                roles.insert(role_name);
                return make_ready_future<>();
            });
        });
    });
}

future <role_set> rest_role_manager::query_granted(std::string_view grantee_name, recursive_role_query m) {
    // Our implementation of roles does not support recursive role query
    return do_with(
            role_set{sstring(grantee_name)},
            [this, grantee_name](role_set &roles) {
                return collect_roles(_qp, grantee_name, roles).then([&roles] { return roles; });
            });
}

future<bool> rest_role_manager::exists(std::string_view role_name) {
    // Used in grant revoke permissions to add permission if role exist
    // but we do not create role for groups so not checking if it exists
    // Also used after authentication to check if user has been well created
    // but user is created by the rest authenticator so not required also
    return make_ready_future<bool>(true);
}

future<bool> rest_role_manager::is_superuser(std::string_view role_name) {
    return find_record(_qp, role_name).then([](std::optional <record> mr) {
        if (mr) {
            record r = *mr;
            return r.is_superuser;
        }
        return false;
    });
}

future<bool> rest_role_manager::can_login(std::string_view role_name) {
    return find_record(_qp, role_name).then([](std::optional <record> mr) {
        if (mr) {
            record r = *mr;
            return r.can_login;
        }
        return false;
    });
}

// Needed for unittest
future<> rest_role_manager::create_or_replace(std::string_view role_name, const role_config& c) const {
    static const sstring query = format("INSERT INTO {} ({}, is_superuser, can_login) VALUES (?, ?, ?)",
                                        meta::roles_table::qualified_name,
                                        meta::roles_table::role_col_name);
    return _qp.execute_internal(
            query,
            consistency_for_role(role_name),
            internal_distributed_query_state(),
            {sstring(role_name), c.is_superuser, c.can_login},
            cql3::query_processor::cache_internal::yes).discard_result();
}

// Needed for unittest
future<> rest_role_manager::create(std::string_view role_name, const role_config &c) {
    return this->create_or_replace(role_name, c);
}

future<>
rest_role_manager::alter(std::string_view role_name, const role_config_update &u) {
    // Role manager only managed update of can_login and is_superuser field
    // Those fields must not be managed by us but set by the rest authenticator when creating user
    return make_ready_future<>();
}

future<> rest_role_manager::drop(std::string_view role_name) {
    throw std::logic_error("Not Implemented");
}

future<> rest_role_manager::grant(std::string_view grantee_name, std::string_view role_name) {
    throw std::logic_error("Not Implemented");
}

future<> rest_role_manager::revoke(std::string_view revokee_name, std::string_view role_name) {
    throw std::logic_error("Not Implemented");
}

future <role_set> rest_role_manager::query_all() {
    static const sstring query = format("SELECT {},member_of from {}",
                                        meta::roles_table::role_col_name,
                                        meta::roles_table::qualified_name);

    // To avoid many copies of a view.
    static const auto role_col_name_string = sstring(meta::roles_table::role_col_name);
    static const auto member_of_col_name_string = sstring("member_of");

    return _qp.execute_internal(
            query,
            db::consistency_level::QUORUM,
            internal_distributed_query_state(),
            cql3::query_processor::cache_internal::yes)
        .then([](::shared_ptr <cql3::untyped_result_set> results) {
            role_set roles;

            std::for_each(
                    results->begin(),
                    results->end(),
                    [&roles] (const cql3::untyped_result_set_row &row) {
                        roles.insert(row.get_as<sstring>(role_col_name_string));
                        if (row.has(member_of_col_name_string)) {
                            for (auto member : row.get_set<sstring>(member_of_col_name_string)) {
                                roles.insert(member);
                            }
                        }
                    });
            return roles;
        });
}


//
// NB: role attribute managment replicated from standard_role_manager.cc
//

future<std::optional<sstring>> rest_role_manager::get_attribute(std::string_view role_name, std::string_view attribute_name) {
    static const sstring query = format("SELECT name, value FROM {} WHERE role = ? AND name = ?", meta::role_attributes_table::qualified_name());
    return _qp.execute_internal(query, {sstring(role_name), sstring(attribute_name)}, cql3::query_processor::cache_internal::yes).then([] (shared_ptr<cql3::untyped_result_set> result_set) {
        if (!result_set->empty()) {
            const cql3::untyped_result_set_row &row = result_set->one();
            return std::optional<sstring>(row.get_as<sstring>("value"));
        }
        return std::optional<sstring>{};
    });
}

future<role_manager::attribute_vals> rest_role_manager::query_attribute_for_all (std::string_view attribute_name) {
    return query_all().then([this, attribute_name] (role_set roles) {
        return do_with(attribute_vals{}, [this, attribute_name, roles = std::move(roles)] (attribute_vals &role_to_att_val) {
            return parallel_for_each(roles.begin(), roles.end(), [this, &role_to_att_val, attribute_name] (sstring role) {
                return get_attribute(role, attribute_name).then([&role_to_att_val, role] (std::optional<sstring> att_val) {
                    if (att_val) {
                        role_to_att_val.emplace(std::move(role), std::move(*att_val));
                    }
                });
            }).then([&role_to_att_val] () {
                return make_ready_future<attribute_vals>(std::move(role_to_att_val));
            });
        });
    });
}

future<> rest_role_manager::set_attribute(std::string_view role_name, std::string_view attribute_name, std::string_view attribute_value) {
    static const sstring query = format("INSERT INTO {} (role, name, value)  VALUES (?, ?, ?)", meta::role_attributes_table::qualified_name());
    return do_with(sstring(role_name), sstring(attribute_name), sstring(attribute_value), [this] (sstring& role_name, sstring &attribute_name,
            sstring &attribute_value) {
        return exists(role_name).then([&role_name, &attribute_name, &attribute_value, this] (bool role_exists) {
            if (!role_exists) {
                throw auth::nonexistant_role(role_name);
            }
            return _qp.execute_internal(query, {sstring(role_name), sstring(attribute_name), sstring(attribute_value)}, cql3::query_processor::cache_internal::yes).discard_result();
        });
    });

}

future<> rest_role_manager::remove_attribute(std::string_view role_name, std::string_view attribute_name) {
    static const sstring query = format("DELETE FROM {} WHERE role = ? AND name = ?", meta::role_attributes_table::qualified_name());
    return do_with(sstring(role_name), sstring(attribute_name), [this] (sstring& role_name, sstring &attribute_name) {
        return exists(role_name).then([&role_name, &attribute_name, this] (bool role_exists) {
            if (!role_exists) {
                throw auth::nonexistant_role(role_name);
            }
            return _qp.execute_internal(query, {sstring(role_name), sstring(attribute_name)}, cql3::query_processor::cache_internal::yes).discard_result();
        });
    });
}
}