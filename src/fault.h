#pragma once
#ifndef CATA_SRC_FAULT_H
#define CATA_SRC_FAULT_H

#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "calendar.h"
#include "memory_fast.h"
#include "translations.h"
#include "type_id.h"

template <typename T> class generic_factory;

class fault;
class fault_fix;
class item;
class JsonObject;
struct requirement_data;

namespace faults
{
void load_fault( const JsonObject &jo, const std::string &src );
void load_fix( const JsonObject &jo, const std::string &src );

// resets faults and fault fixes
void reset();
// finalizes faults and fault fixes
void finalize();
// checks faults and fault fixes
void check();

// @returns faults that can be applied to item \p it
std::vector<fault_id> faults_for_item( const item &it );
} // namespace faults

class fault_fix
{
    public:
        fault_fix_id id = fault_fix_id::NULL_ID();
        translation name;
        time_duration time = 0_seconds;
        translation success_msg; // message to print on applying successfully
        std::map<std::string, std::string> set_variables; // item vars applied to item
        std::map<skill_id, int> skills; // map of skill_id to required level
        std::set<fault_id> faults_removed; // which faults are removed on applying
        std::set<fault_id> faults_added; // which faults are added on applying
        int mod_damage = 0; // mod_damage with this value is called on item applied to
        int mod_degradation = 0; // mod_degradation with this value is called on item applied to

        const requirement_data &get_requirements() const;

        void finalize();

    private:
        void load( const JsonObject &jo, std::string_view src );
        void check() const;
        bool was_loaded = false; // used by generic_factory
        friend class generic_factory<fault_fix>;
        friend class fault;
        shared_ptr_fast<requirement_data> requirements;
};

class fault
{
    public:
        fault_id id = fault_id::NULL_ID();
        // if true the fault can be applied multiple times
        bool stackable = false;

        std::string name() const;
        std::string description() const;
        std::string item_prefix() const;
        const material_id &material_damage() const;
        int get_mod_damage() const;
        bool has_flag( const std::string &flag ) const;

        const std::set<fault_fix_id> &get_fixes() const;

    private:
        friend class generic_factory<fault>;
        friend class fault_fix;

        void load( const JsonObject &jo, std::string_view );
        void check() const;

        bool was_loaded = false; // used by generic_factory
        translation name_;
        translation description_;
        translation item_prefix_; // prefix added to affected item's name
        material_id material_damage_ = material_id::NULL_ID();
        std::set<fault_fix_id> fixes;
        std::set<std::string> flags;
        int mod_damage;
};

#endif // CATA_SRC_FAULT_H
