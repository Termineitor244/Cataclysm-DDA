#pragma once
#ifndef CATA_SRC_CONDITION_H
#define CATA_SRC_CONDITION_H

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "calendar.h"
#include "coords_fwd.h"
#include "dialogue.h"
#include "dialogue_helpers.h"
#include "translation.h"

class JsonObject;
class JsonValue;
enum class var_type : int;
template <typename T> struct enum_traits;

namespace dialogue_data
{
const std::unordered_set<std::string> &simple_string_conds();
const std::unordered_set<std::string> &complex_conds();
} // namespace dialogue_data

enum class jarg {
    member = 1,
    object = 1 << 1,
    string = 1 << 2,
    array = 1 << 3
};

template<>
struct enum_traits<jarg> {
    static constexpr bool is_flag_enum = true;
};

str_or_var get_str_or_var( const JsonValue &jv, std::string_view member, bool required = true,
                           std::string_view default_val = "" );
str_or_var get_str_or_var( const JsonObject &jo, std::string_view member,
                           std::string_view default_val = "" );
translation_or_var get_translation_or_var( const JsonValue &jv, std::string_view member,
        bool required = true, const translation &default_val = {} );
translation_or_var get_translation_or_var( const JsonObject &jo, std::string_view member,
        const translation &default_val = {} );
str_translation_or_var get_str_translation_or_var(
    const JsonValue &jv, std::string_view member, bool required = true,
    std::string_view str_default_val = "", const translation &translation_default_val = {} );
dbl_or_var get_dbl_or_var( const JsonObject &jo, std::string_view member, bool required = true,
                           double default_val = 0.0 );
dbl_or_var_part get_dbl_or_var_part( const JsonValue &jv, std::string_view member,
                                     bool required = true,
                                     double default_val = 0.0 );
duration_or_var get_duration_or_var( const JsonObject &jo, const std::string_view &member,
                                     bool required = true,
                                     time_duration default_val = 0_seconds );
duration_or_var_part get_duration_or_var_part( const JsonValue &jv, const std::string_view &member,
        bool required = true,
        time_duration default_val = 0_seconds );
template<typename T>
T convert_tripoint_from_var( std::optional<var_info> &var, const_dialogue const &d,
                             bool is_npc );
tripoint_abs_ms get_tripoint_ms_from_var( std::optional<var_info> var, const_dialogue const &d,
        bool is_npc );
var_info read_var_info( const JsonObject &jo );
translation_var_info read_translation_var_info( const JsonObject &jo );
void write_var_value( var_type type, const std::string &name, dialogue *d,
                      const std::string &value, int call_depth = 0 );
void write_var_value( var_type type, const std::string &name, dialogue *d,
                      double value );
void write_var_value( var_type type, const std::string &name, const_dialogue const &d,
                      const std::string &value );
std::string get_talk_varname( const JsonObject &jo, std::string_view member,
                              bool check_value, dbl_or_var &default_val );
std::string get_talk_var_basename( const JsonObject &jo, std::string_view member,
                                   bool check_value );
// the truly awful declaration for the conditional_t loading helper_function
void read_condition( const JsonObject &jo, const std::string &member_name,
                     std::function<bool( const_dialogue const & )> &condition, bool default_val );

void finalize_conditions();

/**
 * A condition for a response spoken by the player.
 * This struct only adds the constructors which will load the data from json
 * into a lambda, stored in the std::function object.
 * Invoking the function operator with a dialog reference (so the function can access the NPC)
 * returns whether the response is allowed.
 */
struct conditional_t {
    public:
        using func = std::function<bool( const_dialogue const & )>;

        conditional_t() = default;
        explicit conditional_t( std::string_view type );
        explicit conditional_t( const JsonObject &jo );

        static std::function<std::string( const_dialogue const & )> get_get_string( const JsonObject &jo );
        static std::function<translation( const_dialogue const & )> get_get_translation(
            const JsonObject &jo );
        static double get_legacy_dbl( const_dialogue const &d, std::string_view checked_value, char scope );
        static void set_legacy_dbl( dialogue &d, double input, std::string_view checked_value, char scope );
        bool operator()( const_dialogue const &d ) const {
            if( !condition ) {
                return false;
            }
            return condition( d );
        }

    private:
        func condition;
};

#endif // CATA_SRC_CONDITION_H
