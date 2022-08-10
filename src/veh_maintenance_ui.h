#pragma once
#ifndef CATA_SRC_VEH_MAINTENANCE_UI
#define CATA_SRC_VEH_MAINTENANCE_UI

#include "cursesdef.h"
#include "point.h"
#include "ui.h"
#include "ui_manager.h"
#include "vehicle.h"

class veh_ui_maintenance
{
    public:
        veh_ui_maintenance( vehicle &vehicle );

        /// Starts vehicle ui loop, runs until canceled or activity is selected and returned
        /// @param vehicle Vehicle to handle
        /// @return Selected activity or player_activity( activity_id::NULL_ID() ) if cancelled
        player_activity start( const tripoint &position );

    private:
        ui_adaptor ui;
        input_context ctxt;

        catacurses::window panel_l;
        catacurses::window panel_r;

        // vehicle being worked on
        vehicle &veh;

        // cursor handling
        std::set<tripoint> cursor_allowed; // all points the cursor is allowed to be on
        tripoint cursor_pos;
        tripoint get_cursor_pos() const;
        bool set_cursor_pos( const tripoint &new_pos );
        void draw_cursor() const;
        bool handle_cursor_movement( const std::string &action );

        /// Returns all parts under cursor (no filtering)
        std::vector<vpart_reference> parts_under_cursor() const;

        // break glass^W^W delete this in case multi-level vehicles are a thing
        const bool allow_zlevel_shift = false;

        void init_windows();
        void init_input();

        void on_resize( ui_adaptor &adaptor );
        void on_redraw( ui_adaptor &adaptor ) const;

        const int min_panel_width = 32;
        void draw_info_panel( const catacurses::window &w ) const;
        void draw_part_panel( const catacurses::window &w ) const;

        // doing actual "maintenance"
        std::optional<vpart_reference> select_part_at_cursor(
            const std::string &title, const std::string &extra_key,
            const std::function<std::string( const vpart_reference & )> predicate,
            const std::optional<vpart_reference> preselect ) const;

        player_activity remove_part( vpart_reference vpr ) const;
        void change_part_shape( vpart_reference vpr ) const;

        // caches
        void update_caches();
        // cache for maximum weight_capacity of available jacking equipment
        units::mass max_jack;
        // cache for maximum weight_capacity of available lifting equipment
        units::mass max_lift;

        // checks
        std::pair<bool, std::string> calc_lift_requirements( const vpart_info &vpi ) const;
        bool format_reqs( const catacurses::window &w, std::string &msg, const requirement_data &reqs,
                          const std::map<skill_id, int> &skills, int moves ) const;

        // @returns empty string if removal is available or the missing requirement as string
        std::string can_remove_part( const vehicle_part &sel_vehicle_part ) const;
};

#endif // CATA_SRC_VEH_UI_MAINTENANCE
