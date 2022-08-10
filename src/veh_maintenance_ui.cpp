#include "veh_maintenance_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ui.h"
#include "ui_manager.h"
#include "units.h"
#include "units_utility.h"
#include "value_ptr.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "output.h"
#include "panels.h"
#include "point.h"
#include "map.h"
#include "input.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "game.h"
#include "game_constants.h"
#include "debug.h"
#include "color.h"
#include "cursesdef.h"
#include "character.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "avatar.h"
#include "options.h"
#include "vpart_position.h"
#include "veh_type.h"
#include "veh_utils.h"
#include "vehicle_selector.h"
#include "skill.h"

#pragma optimize("", off)

static const activity_id ACT_VEHICLE( "ACT_VEHICLE" );

static const trait_id trait_BADBACK( "BADBACK" );
static const trait_id trait_DEBUG_HS( "DEBUG_HS" );
static const trait_id trait_STRONGBACK( "STRONGBACK" );

static const quality_id qual_JACK( "JACK" );
static const quality_id qual_LIFT( "LIFT" );
static const quality_id qual_SELF_JACK( "SELF_JACK" );

veh_ui_maintenance::veh_ui_maintenance( vehicle &v ): veh( v )
{
    const auto cursor_allowed_at = [&veh = this->veh]( const tripoint & p ) {
        const map &here = get_map();
        const optional_vpart_position veh_at_p = here.veh_at( p );
        if( veh_at_p && &veh_at_p->vehicle() != &veh ) {
            return false; // block tiles other vehicles stand on
        }
        if( here.impassable_ter_furn( p ) ) {
            return false; // block tiles that have terrain in the way
        }
        // TODO: block tiles player/npcs stand on in case of board/OBSTACLE installation?
        return true;
    };

    for( const vpart_reference &part : veh.get_all_parts() ) {
        cursor_allowed.insert( part.pos() );
    }

    // copy to allow writes
    for( const tripoint &center : std::set<tripoint>( cursor_allowed ) ) {
        for( const tripoint &p : closest_points_first( center, 1 ) ) {
            if( cursor_allowed_at( p ) ) {
                cursor_allowed.insert( p );
            }
        }
    }
}

void veh_ui_maintenance::update_caches()
{
    Character &player_character = get_player_character();

    int self_jack_quality = 0;
    for( const vpart_reference &vp : veh.get_avail_parts( "SELF_JACK" ) ) {
        self_jack_quality = std::max( self_jack_quality,
                                      vp.part().get_base().get_quality( qual_SELF_JACK ) );
    }
    const int max_jack_quality = std::max( {
        self_jack_quality,
        player_character.max_quality( qual_JACK ),
        player_character.is_mounted() ? player_character.mounted_creature->mech_str_addition() + 10 : 0,
        map_selector( player_character.pos(), PICKUP_RANGE ).max_quality( qual_JACK ),
        vehicle_selector( player_character.pos(), 2, true, veh ).max_quality( qual_JACK )
    } );
    max_jack = lifting_quality_to_mass( max_jack_quality );
    max_lift = get_player_character().best_nearby_lifting_assist( get_cursor_pos() );
}

player_activity veh_ui_maintenance::start( const tripoint &p )
{
    map &here = get_map();
    avatar &you = get_avatar();
    on_out_of_scope cleanup( []() {
        get_map().invalidate_map_cache( get_avatar().view_offset.z );
    } );
    restore_on_out_of_scope<tripoint> view_offset_prev( you.view_offset );

    if( !set_cursor_pos( p ) ) {
        set_cursor_pos( veh.global_part_pos3( 0 ) );
    }

    const auto target_ui_cb = make_shared_fast<game::draw_callback_t>(
    [&]() {
        draw_cursor();
    } );
    g->add_draw_callback( target_ui_cb );
    g->reset_wait_popup();

    ui_adaptor ui;
    ui.on_screen_resize( std::bind( &veh_ui_maintenance::on_resize, this, std::placeholders::_1 ) );
    ui.on_redraw( std::bind( &veh_ui_maintenance::on_redraw, this, std::placeholders::_1 ) );
    ui.mark_resize();

    while( true ) {
        g->invalidate_main_ui_adaptor();
        ui_manager::redraw();

        std::string action = ctxt.handle_input( get_option<int>( "EDGE_SCROLL" ) );

        if( handle_cursor_movement( action ) || ( action == "HELP_KEYBINDINGS" ) ) {
            continue;
            //} else if( action == "CONFIRM" || action == "SELECT" ) {
            //    // SELECT here means same tile was clicked twice in a row
            //    const optional_vpart_position ovp = here.veh_at( cursor_pos );
            //    if( ovp ) {
            //        return player_activity();
            //    }
            //    continue; // can't select where no vehicle parts are
        } else if( action == "CONFIRM" ) {
            player_activity res;
            res.name = "go legacy";
            return res;
        } else if( action == "REMOVE" ) {
            std::optional<vpart_reference> part = select_part_at_cursor( _( "Choose part to remove" ), "o",
            [this]( const vpart_reference & vp ) -> std::string {
                return can_remove_part( vp.part() );
            }, std::nullopt );
            if( part.has_value() ) {
                return remove_part( part.value() );
            }
            continue;
        } else if( action == "CHANGE_SHAPE" ) {
            std::optional<vpart_reference> part;
            do {
                part = select_part_at_cursor( _( "Choose part to change shape" ), "p",
                []( const vpart_reference & vp ) {
                    return vp.info().variants.size() > 1 ? "" : "No other shapes";
                }, part );
                if( part.has_value() ) {
                    change_part_shape( part.value() );
                }
            } while( part.has_value() );
        } else if( action == "QUIT" ) {
            return player_activity();
        } else {
            debugmsg( "here be dragons" );
            return player_activity();
        }
    }
}

std::vector<vpart_reference> veh_ui_maintenance::parts_under_cursor() const
{
    std::vector<vpart_reference> res;
    // TODO: tons of methods getting parts from vehicle but all of them seem inadequate?
    for( size_t part_idx = 0; part_idx < veh.part_count_real(); part_idx++ ) {
        const vehicle_part &p = veh.part( part_idx );
        if( veh.global_part_pos3( p ) == get_cursor_pos() && !p.is_fake ) {
            res.push_back( vpart_reference( veh, part_idx ) );
        }
    }
    return res;
}

class part_select_uilist_callback : public uilist_callback
{
    private:
        uilist &ui;
        const std::string &extra_key;
    public:
        part_select_uilist_callback( uilist &ui, const std::string &extra_key ): ui( ui ),
            extra_key( extra_key ) {}
        virtual bool key( const input_context &, const input_event &key, int /*entnum*/, uilist * ) {
            if( key.text == extra_key && ui.entries[ui.selected].enabled ) {
                ui.ret = ui.selected;
                return true;
            } else {
                return false;
            }
        }
};

std::optional<vpart_reference> veh_ui_maintenance::select_part_at_cursor(
    const std::string &title, const std::string &extra_key,
    const std::function<std::string( const vpart_reference & )> predicate,
    const std::optional<vpart_reference> preselect ) const
{
    const std::vector<vpart_reference> parts = parts_under_cursor();
    if( parts.empty() ) {
        return std::nullopt;
    }

    uilist menu;
    menu.w_x_setup = TERMX / 8;

    for( const vpart_reference &pt : parts ) {
        std::string predicate_result = predicate( pt );
        uilist_entry entry( -1, true, MENU_AUTOASSIGN, pt.part().name() + " " + predicate_result,
                            "", std::to_string( pt.part().degradation() ) );
        entry.retval = predicate_result.empty() ? menu.entries.size() : -2;
        if( preselect && preselect->part_index() == pt.part_index() ) {
            menu.selected = menu.entries.size();
        }
        menu.entries.push_back( entry );
    }
    menu.text = title;
    part_select_uilist_callback cb( menu, extra_key );
    menu.callback = &cb;
    menu.query();

    return menu.ret >= 0 ? std::optional<vpart_reference>( parts[menu.ret] ) : std::nullopt;
}

player_activity veh_ui_maintenance::remove_part( vpart_reference vpr ) const
{
    const vehicle_part &pt = vpr.part();
    const vpart_info &vp = pt.info();

    avatar &player_character = get_avatar();
    int time = vp.removal_time( player_character );
    if( player_character.has_trait( trait_DEBUG_HS ) ) {
        time = 1;
    }
    player_activity res( ACT_VEHICLE, time, static_cast<int>( 'o' ) ); // TODO: remove weird cast

    // if we're working on an existing part, use that part as the reference point
    // otherwise (e.g. installing a new frame), just use part 0
    const point q = veh.coord_translate( pt.mount );
    const vehicle_part *vpt = &pt;
    map &here = get_map();
    for( const tripoint &p : veh.get_points( true ) ) {
        res.coord_set.insert( here.getabs( p ) );
    }
    // wtf none of this gets used
    res.values.push_back( here.getabs( veh.global_pos3() ).x + q.x );    // values[0]
    res.values.push_back( here.getabs( veh.global_pos3() ).y + q.y );    // values[1]
    res.values.push_back( get_cursor_pos().x );                          // values[2]
    res.values.push_back( get_cursor_pos().y );                          // values[3]
    res.values.push_back( 0 );                                           // values[4]
    res.values.push_back( 0 );                                           // values[5]
    res.values.push_back( veh.index_of_part( vpt ) );                // values[6]
    res.str_values.push_back( vp.get_id().str() );
    res.str_values.push_back( std::string() );
    res.targets.emplace_back( item_location() );

    return res;
}

void veh_ui_maintenance::change_part_shape( vpart_reference vpr ) const
{
    vehicle_part &part = vpr.part();
    const vpart_info &vpi = part.info();
    veh_menu menu( this->veh, _( "Choose cosmetic variant:" ) );

    do {
        menu.reset( false );

        for( const auto &[vvid, vv] : vpi.variants ) {
            menu.add( vv.get_label() )
            .keep_menu_open()
            .skip_locked_check()
            .skip_theft_check()
            .location( veh.global_part_pos3( part ) )
            .select( part.variant == vvid )
            .symbol( vv.get_symbol_curses( 0_degrees, false ) )
            .symbol_color( vpi.color )
            .on_select( [&part, variant_id = vvid]() {
                part.variant = variant_id;
            } )
            .on_submit( []() {} ); // noop, on_select does the work
        }

        // An ordering of the line drawing symbols that does not result in
        // connecting when placed adjacent to each other vertically.
        menu.sort( []( const veh_menu_item & a, const veh_menu_item & b ) {
            const static std::map<int, int> symbol_order = {
                { LINE_XOXO, 0 }, { LINE_OXOX, 1 }, { LINE_XOOX, 2 }, { LINE_XXOO, 3 },
                { LINE_XXXX, 4 }, { LINE_OXXO, 5 }, { LINE_OOXX, 6 },
            };
            const auto a_iter = symbol_order.find( a._symbol );
            const auto b_iter = symbol_order.find( b._symbol );
            if( a_iter != symbol_order.end() ) {
                return ( b_iter == symbol_order.end() ) || ( a_iter->second < b_iter->second );
            } else {
                return ( b_iter == symbol_order.end() ) && ( a._symbol < b._symbol );
            }
        } );
    } while( menu.query() );
}

tripoint veh_ui_maintenance::get_cursor_pos() const
{
    return cursor_pos;
}

bool veh_ui_maintenance::set_cursor_pos( const tripoint &new_pos )
{
    avatar &you = get_avatar();

    int z = std::max( { new_pos.z, -fov_3d_z_range, -OVERMAP_DEPTH } );
    z = std::min( {z, fov_3d_z_range, OVERMAP_HEIGHT } );

    if( !allow_zlevel_shift ) {
        z = cursor_pos.z;
    }
    tripoint target_pos( new_pos.xy(), z );

    if( cursor_allowed.find( target_pos ) == cursor_allowed.cend() ) {
        return false;
    }

    if( z != cursor_pos.z ) {
        get_map().invalidate_map_cache( z );
    }
    cursor_pos = target_pos;
    you.view_offset = cursor_pos - you.pos();
    update_caches();
    return true;
}

bool veh_ui_maintenance::handle_cursor_movement( const std::string &action )
{
    if( action == "MOUSE_MOVE" || action == "TIMEOUT" ) {
        tripoint edge_scroll = g->mouse_edge_scrolling_terrain( ctxt );
        set_cursor_pos( get_cursor_pos() + edge_scroll );
    } else if( const std::optional<tripoint> delta = ctxt.get_direction( action ) ) {
        set_cursor_pos( get_cursor_pos() + *delta ); // move cursor with directional keys
    } else if( action == "zoom_in" ) {
        g->zoom_in();
    } else if( action == "zoom_out" ) {
        g->zoom_out();
    } else if( action == "SELECT" ) {
        const std::optional<tripoint> mouse_pos = ctxt.get_coordinates( g->w_terrain );
        if( !mouse_pos ) {
            return false;
        }
        if( get_cursor_pos() != *mouse_pos ) {
            set_cursor_pos( *mouse_pos );
        }
    } else if( action == "LEVEL_UP" ) {
        set_cursor_pos( get_cursor_pos() + tripoint_above );
    } else if( action == "LEVEL_DOWN" ) {
        set_cursor_pos( get_cursor_pos() + tripoint_below );
    } else {
        return false;
    }

    return true;
}

void veh_ui_maintenance::draw_cursor() const
{
    const avatar &you = get_avatar();
    g->draw_cursor( you.pos() + you.view_offset );
}

void veh_ui_maintenance::on_resize( ui_adaptor &adaptor )
{
    init_windows();
    init_input();
    ui.position_from_window( panel_l );
}

void veh_ui_maintenance::init_windows()
{
    panel_manager &panel_mgr = panel_manager::get_manager();

    const int panel_l_width = std::max( panel_mgr.get_width_left(), min_panel_width );
    panel_l = catacurses::newwin( TERMY, panel_l_width, point_zero );

    const int panel_r_width = std::max( panel_mgr.get_width_right(), min_panel_width );
    panel_r = catacurses::newwin( TERMY, panel_r_width, point( TERMX - panel_r_width, 0 ) );
}

void veh_ui_maintenance::init_input()
{
    ctxt = input_context( "VEH_INTERACT", keyboard_mode::keycode );
    ctxt.set_iso( true );
    ctxt.register_directions();
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "LEVEL_UP" );
    ctxt.register_action( "LEVEL_DOWN" );
    ctxt.register_action( "REMOVE" );
    ctxt.register_action( "CHANGE_SHAPE" );
    ctxt.register_action( "zoom_out" );
    ctxt.register_action( "zoom_in" );
}

void draw_panel_heading( const catacurses::window &w, int y, const std::string &title,
                         nc_color color = c_cyan )
{
    const int width = catacurses::getmaxx( w );
    center_print( w, y, color, "[ " + title + " ]" );
}

void draw_panel_stat( const catacurses::window &w, int y, const std::string &text,
                      nc_color color = c_cyan )
{
    const int width = catacurses::getmaxx( w );
    center_print( w, y, color, text );
}

void veh_ui_maintenance::draw_info_panel( const catacurses::window &w ) const
{
    const int ww = catacurses::getmaxx( w );
    const int wh = catacurses::getmaxy( w );
    int y = 1;

    werase( w );
    draw_border( w );
    center_print( w, y++, c_light_blue, "[ " + std::string( _( "Information" ) ) + " ]" );

    const auto vel_to_str = []( const double vel ) -> std::string {
        return std::to_string( static_cast<int>( convert_velocity( vel, VU_VEHICLE ) ) );
    };

    units::volume total_cargo = 0_ml;
    units::volume free_cargo = 0_ml;
    for( const vpart_reference &vp : veh.get_any_parts( "CARGO" ) ) {
        const size_t p = vp.part_index();
        total_cargo += veh.max_volume( p );
        free_cargo += veh.free_volume( p );
    }

    struct info_panel_entry {
        translation title;
        std::string value;
        nc_color value_color = c_light_green;
        std::string units = "";
        std::string value2 = "";
        nc_color value2_color = c_light_green;
    };

    std::vector<info_panel_entry> info_entries;
    const auto info_separator = [&info_entries]() {
        info_entries.push_back( {no_translation( "" ), "|---|"} );
    };
    info_entries.push_back( { to_translation( "Vehicle", "Name" ), veh.name } );
    info_entries.push_back( {
        to_translation( "Vehicle", "Safe/Top speed" ),
        vel_to_str( veh.safe_ground_velocity( false ) ), c_light_green,
        std::string( velocity_units( VU_VEHICLE ) ),
        vel_to_str( veh.max_ground_velocity( false ) ), c_light_red,
    } );
    info_entries.push_back( {
        to_translation( "Vehicle", "Acceleration" ),
        vel_to_str( veh.acceleration( false ) ), c_light_blue,
        std::string( velocity_units( VU_VEHICLE ) ) + "/s",
    } );
    info_entries.push_back( {
        to_translation( "Vehicle", "Mass" ),
        std::to_string( static_cast<int>( convert_weight( veh.total_mass() ) ) ), c_light_blue,
        std::string( weight_units() ),
    } );
    info_entries.push_back( {
        to_translation( "Vehicle", "Cargo" ),
        format_volume( total_cargo - free_cargo, 5, nullptr, nullptr ), c_light_blue,
        std::string( volume_units_abbr() ),
        format_volume( total_cargo, 5, nullptr, nullptr ), c_light_blue,
    } );
    info_separator();

    for( const info_panel_entry &e : info_entries ) {
        if( e.value == "|---|" ) {
            mvwputch( w, point( 0, y ), c_light_gray, LINE_XXXO );
            for( int x = 1; x < ww - 1; x++ ) {
                mvwputch( w, point( x, y ), c_light_gray, LINE_OXOX );
            }
            mvwputch( w, point( ww - 1, y ), c_light_gray, LINE_XOXX );
            continue;
        }

        std::string name_str = e.title.translated() + ": ";
        std::string value_str = e.value;
        if( !e.value2.empty() ) {
            value_str += " / " + e.value2;
        }
        if( !e.units.empty() ) {
            value_str += " " + e.units;
        }
        const int margin = 2;
        const int name_len = utf8_width( name_str );
        const int value_len = utf8_width( value_str );
        const int text_width = name_len + value_len;
        const int spacing = std::max( 0, ww - text_width - 2 * margin );

        if( text_width >= ww - 2 * margin ) {
            name_str = trim_by_length( name_str, ww - value_len - 2 * margin );
        }

        nc_color dummy = c_light_gray;
        std::string line = name_str + std::string( spacing, ' ' ) + colorize( e.value, e.value_color );
        if( !e.value2.empty() ) {
            line += " / " + colorize( e.value2, e.value2_color );
        }
        if( !e.units.empty() ) {
            line += " " + e.units;
        }
        print_colored_text( w, point( margin, y++ ), dummy, dummy, line );
    }

    wnoutrefresh( w );
}

void veh_ui_maintenance::draw_part_panel( const catacurses::window &w ) const
{
    int ww = catacurses::getmaxx( w );
    int wh = catacurses::getmaxy( w );
    int y = 0;

    werase( w );
    draw_border( w );
    center_print( w, y, c_cyan, "[ " + std::string( _( "Parts here" ) ) + " ]" );

    map &here = get_map();
    optional_vpart_position veh_opt = here.veh_at( get_cursor_pos() );
    if( !veh_opt ) {
        center_print( w, 2, c_red, _( "No vehicle parts under cursor" ) );
        wnoutrefresh( w );
        return;
    }
    const vehicle &veh = veh_opt->vehicle();
    veh.print_part_list( w, 2, wh, ww, veh_opt->part_index() );
    nc_color fg = c_light_gray;
    const std::string keybinds_string = string_format( "[ " + std::string(
                                            _( "Press <color_light_green>%s</color> to view and alter keybindings." ) ) + " ]",
                                        ctxt.get_desc( "HELP_KEYBINDINGS" ) );
    right_print( w, wh - 1, 1, fg, keybinds_string );

    wnoutrefresh( w );
}

void veh_ui_maintenance::on_redraw( ui_adaptor &adaptor ) const
{
    draw_info_panel( panel_l );
    draw_part_panel( panel_r );
}

//player_activity veh_ui_maintenance::serialize_activity()
//{

//const auto *pt = sel_vehicle_part;
//const auto *vp = sel_vpart_info;

//if( sel_cmd == 'q' || sel_cmd == ' ' || !vp ) {
//return player_activity();
//}

//avatar &player_character = get_avatar();
//int time = 1000;
//switch( sel_cmd ) {
//case 'i':
//    time = vp->install_time( player_character );
//    break;
//case 'r':
//    if( pt != nullptr ) {
//        if( pt->is_broken() ) {
//            time = vp->install_time( player_character );
//        } else if( pt->base.max_damage() > 0 ) {
//            time = vp->repair_time( player_character ) * ( pt->base.damage() - pt->base.damage_floor(
//                false ) ) / pt->base.max_damage();
//        }
//    }
//    break;
//case 'o':
//    time = vp->removal_time( player_character );
//    break;
//default:
//    break;
//}
//if( player_character.has_trait( trait_DEBUG_HS ) ) {
//    time = 1;
//}
//player_activity res( ACT_VEHICLE, time, static_cast<int>( sel_cmd ) );

//// if we're working on an existing part, use that part as the reference point
//// otherwise (e.g. installing a new frame), just use part 0
//const point q = veh->coord_translate( pt ? pt->mount : veh->part( 0 ).mount );
//const vehicle_part *vpt = pt ? pt : &veh->part( 0 );
//map &here = get_map();
//for( const tripoint &p : veh->get_points( true ) ) {
//    res.coord_set.insert( here.getabs( p ) );
//}
//res.values.push_back( here.getabs( veh->global_pos3() ).x + q.x );    // values[0]
//res.values.push_back( here.getabs( veh->global_pos3() ).y + q.y );    // values[1]
//res.values.push_back( dd.x );   // values[2]
//res.values.push_back( dd.y );   // values[3]
//res.values.push_back( -dd.x );   // values[4]
//res.values.push_back( -dd.y );   // values[5]
//res.values.push_back( veh->index_of_part( vpt ) ); // values[6]
//res.str_values.push_back( vp->get_id().str() );
//res.str_values.push_back( sel_vpart_variant );
//res.targets.emplace_back( std::move( target ) );

//return res;
//}


bool veh_ui_maintenance::format_reqs( const catacurses::window &w, std::string &msg,
                                      const requirement_data &reqs,
                                      const std::map<skill_id, int> &skills, int moves ) const
{
    Character &player_character = get_player_character();
    const inventory &inv = player_character.crafting_inventory();
    bool ok = reqs.can_make_with_inventory( inv, is_crafting_component );

    const auto status_color = []( bool status ) {
        return status ? "<color_green>" : "<color_red>";
    };

    msg += _( "<color_white>Time required:</color>\n" );
    msg += "> " + to_string_approx( time_duration::from_moves( moves ) ) + "\n";

    msg += _( "<color_white>Skills required:</color>\n" );
    for( const auto &e : skills ) {
        bool hasSkill = player_character.get_knowledge_level( e.first ) >= e.second;
        if( !hasSkill ) {
            ok = false;
        }
        //~ %1$s represents the internal color name which shouldn't be translated, %2$s is skill name, and %3$i is skill level
        msg += string_format( _( "> %1$s%2$s %3$i</color>\n" ), status_color( hasSkill ),
                              e.first.obj().name(), e.second );
    }
    if( skills.empty() ) {
        //~ %1$s represents the internal color name which shouldn't be translated, %2$s is the word "NONE"
        msg += string_format( "> %1$s%2$s</color>", status_color( true ), _( "NONE" ) ) + "\n";
    }
    const int panel_l_width = catacurses::getmaxx( w );
    auto comps = reqs.get_folded_components_list( panel_l_width - 2, c_white, inv,
                 is_crafting_component );
    for( const std::string &line : comps ) {
        msg += line + "\n";
    }
    auto tools = reqs.get_folded_tools_list( panel_l_width - 2, c_white, inv );
    for( const std::string &line : tools ) {
        msg += line + "\n";
    }

    return ok;
}

std::string veh_ui_maintenance::can_remove_part( const vehicle_part &vp ) const
{
    const vpart_info &vpi = vp.info();
    const Character &worker = get_player_character();
    const bool simple_part = vpi.has_flag( "SIMPLE_PART" );
    const bool smash_remove = vpi.has_flag( "SMASH_REMOVE" );
    const bool no_modify = veh.has_part( "NO_MODIFY_VEHICLE" );
    const bool no_uninstall = vpi.has_flag( "NO_UNINSTALL" );

    if( worker.has_trait( trait_DEBUG_HS ) ) {
        return std::string();
    }

    if( no_modify && !simple_part && !smash_remove ) {
        return _( "This vehicle cannot be modified in this way.\n" );
    } else if( no_uninstall ) {
        return _( "This part cannot be uninstalled.\n" );
    }

    std::string reason;
    if( !format_reqs( panel_r, reason, vpi.removal_requirements(), vpi.removal_skills,
                      vpi.removal_time( worker ) ) ) {
        return reason;
    }

    std::pair<bool, std::string> res = calc_lift_requirements( vpi );
    if( !res.first ) {
        return res.second;
    }

    if( !veh.can_unmount( veh.index_of_part( &vp ), reason ) ) {
        //~ %1$s represents the internal color name which shouldn't be translated, %2$s is pre-translated reason
        return string_format( _( "> <color_red>%1$s</color>" ), reason );
    }

    return std::string();
}

static int get_required_jack_quality( const vehicle &veh )
{
    // clamp JACK requirements to support arbitrarily large vehicles
    const units::mass mass = std::min( veh.total_mass(), 8500_kilogram );
    return static_cast<int>( std::ceil( mass / lifting_quality_to_mass( 1 ) ) );
}

std::pair<bool, std::string> veh_ui_maintenance::calc_lift_requirements(
    const vpart_info &vpi ) const
{
    int lvl = 0;
    int str = 0;
    quality_id qual;
    bool use_aid = false;
    bool use_str = false;
    bool ok = true;
    std::string nmsg;
    avatar &player_character = get_avatar();

    if( vpi.has_flag( "NEEDS_JACKING" ) ) {
        qual = qual_JACK;
        lvl = get_required_jack_quality( veh );
        str = veh.lift_strength();
        use_aid = max_jack >= lifting_quality_to_mass( lvl );
        use_str = player_character.can_lift( veh );
    } else {
        item base( vpi.base_item );
        qual = qual_LIFT;
        lvl = std::ceil( units::quantity<double, units::mass::unit_type>( base.weight() ) /
                         lifting_quality_to_mass( 1 ) );
        str = base.lift_strength();
        use_aid = max_lift >= base.weight();
        use_str = player_character.can_lift( base );
    }

    if( !( use_aid || use_str ) ) {
        ok = false;
    }

    std::string str_suffix;
    int lift_strength = player_character.get_lift_str();
    int total_lift_strength = lift_strength + player_character.get_lift_assist();
    int total_base_strength = player_character.get_arm_str() + player_character.get_lift_assist();

    if( player_character.has_trait( trait_STRONGBACK ) && total_lift_strength >= str &&
        total_base_strength < str ) {
        str_suffix = string_format( _( "(Strong Back helped, giving +%d strength)" ),
                                    lift_strength - player_character.get_str() );
    } else if( player_character.has_trait( trait_BADBACK ) && total_base_strength >= str &&
               total_lift_strength < str ) {
        str_suffix = string_format( _( "(Bad Back reduced usable strength by %d)" ),
                                    lift_strength - player_character.get_str() );
    }

    nc_color aid_color = use_aid ? c_green : ( use_str ? c_dark_gray : c_red );
    nc_color str_color = use_str ? c_green : ( use_aid ? c_dark_gray : c_red );
    const auto helpers = player_character.get_crafting_helpers();
    //~ %1$s is quality name, %2$d is quality level
    std::string aid_string = string_format( _( "1 tool with %1$s %2$d" ), qual.obj().name, lvl );

    std::string str_string = !helpers.empty()
                             ? string_format( _( "strength ( assisted ) %d %s" ), str, str_suffix )
                             : string_format( _( "strength %d %s" ), str, str_suffix );

    nmsg += string_format( _( "> %1$s <color_white>OR</color> %2$s" ),
                           colorize( aid_string, aid_color ),
                           colorize( str_string, str_color ) ) + "\n";

    return std::make_pair( ok, nmsg );
}

#pragma optimize("", on)
