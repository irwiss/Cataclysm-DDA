#include "vehicle.h" // IWYU pragma: associated

#include <string>

#include "activity_actor_definitions.h"
#include "map.h"
#include "map_iterator.h"
#include "veh_type.h"
#include "veh_utils.h"
#include "output.h"

#pragma optimize("", off)

/// @returns all mount mounts of flatbed parts from vehicle
static std::vector<point> get_flatbed_mount_points( const vehicle &veh )
{
    std::set<point> flatbed_pts;
    for( const vpart_reference &vpr : veh.get_all_parts() ) {
        if( vpr.part().removed ) {
            continue;
        }
        const cata::optional<vpart_reference> ovr = vpr.part_with_feature( "FLATBED", true );
        if( !ovr || ovr->part().has_flag( vehicle_part::carrying_flag ) ) {
            continue;
        }
        flatbed_pts.emplace( ovr->mount() );
    }
    return std::vector<point>( flatbed_pts.begin(), flatbed_pts.end() );
}

/// @returns all part mounts from vehicle
static std::vector<point> get_all_part_mounts( const vehicle &veh )
{
    std::set<point> mount_pts;
    for( const vpart_reference &p : veh.get_all_parts() ) {
        if( p.part().removed ) {
            continue;
        }
        mount_pts.emplace( p.mount() );
    }
    return std::vector<point>( mount_pts.begin(), mount_pts.end() );
}

static std::vector<vehicle *> get_near_vehicles( std::vector<tripoint> pts, const vehicle &exclude )
{
    std::set<vehicle *> results;
    for( const tripoint &origin : pts ) {
        for( const tripoint &p : get_map().points_in_radius( origin, 1 ) ) {
            const optional_vpart_position ovp = get_map().veh_at( p );
            if( !ovp || &ovp->vehicle() == &exclude ) {
                continue;
            }
            results.emplace( &ovp->vehicle() );
        }
    }
    return std::vector<vehicle *>( results.begin(), results.end() );
}

cata::optional<point> vehicle_fits_on_flatbed( const std::vector<point> &slots,
        const vehicle &flatbed, const vehicle &candidate )
{
    if( candidate.face.dir() != flatbed.face.dir() ) {
        return cata::nullopt;
    }

    const std::vector<point> mounts = get_all_part_mounts( candidate );

    std::set<point> _slots( slots.begin(), slots.end() );
    int min_x = std::numeric_limits<int>::max();
    int min_y = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();
    int max_y = std::numeric_limits<int>::min();
    for( const point &p : mounts ) {
        min_x = std::min( min_x, p.x );
        min_y = std::min( min_y, p.y );
        max_x = std::max( max_x, p.x );
        max_y = std::max( max_y, p.y );
    }

    std::vector<point> candidate_offsets;

    constexpr int MAX_VEHICLE_SIZE = 24;
    for( int ox = min_x - MAX_VEHICLE_SIZE; ox <= max_x + MAX_VEHICLE_SIZE; ox++ ) {
        for( int oy = min_y - MAX_VEHICLE_SIZE; oy <= max_y + MAX_VEHICLE_SIZE; oy++ ) {
            candidate_offsets.emplace_back( ox, oy );
        }
    }

    const point mid( ( max_x - min_x ) / 2, ( max_y - min_y ) / 2 );
    std::sort( candidate_offsets.begin(), candidate_offsets.end(),
    [&mid]( const point & a, const point & b ) {
        return a.distance( mid ) < b.distance( mid );
    } );

    for( const point &offset : candidate_offsets ) {
        int fit_count = 0;
        for( const point &p : mounts ) {
            if( _slots.count( p - offset ) > 0 ) {
                fit_count++;
            }
        }
        if( fit_count == mounts.size() ) {
            return offset;
        }
    }
    return cata::nullopt;
}

std::map<vehicle *, std::pair<point, std::string>> vehicle::get_vehicles_available_to_load() const
{
    map &here = get_map();
    std::map<vehicle *, std::pair<point, std::string>> result;
    const std::vector<point> flatbed_mounts = get_flatbed_mount_points( *this );
    if( flatbed_mounts.empty() ) {
        return {};
    }
    int min_flatbed_x = std::numeric_limits<int>::max();
    int min_flatbed_y = std::numeric_limits<int>::max();
    int max_flatbed_y = std::numeric_limits<int>::min();
    std::vector<tripoint> flatbed_tripoints;
    for( const point &p : flatbed_mounts ) {
        min_flatbed_x = std::min( min_flatbed_x, p.x );
        min_flatbed_y = std::min( min_flatbed_y, p.y );
        max_flatbed_y = std::max( max_flatbed_y, p.y );
        flatbed_tripoints.emplace_back( mount_to_tripoint( p ) );
    }
    std::set<vehicle *> candidates;
    for( int y = min_flatbed_y; y <= max_flatbed_y; y++ ) {
        const tripoint pos = global_pos3() + coord_translate( point( min_flatbed_x - 1, y ) );
        if( const optional_vpart_position ovp = here.veh_at( pos ) ) {
            candidates.emplace( &ovp->vehicle() );
        }
    }
    for( vehicle *candidate : candidates ) {
        cata::optional<point> offset = vehicle_fits_on_flatbed( flatbed_mounts, *this, *candidate );
        std::string hint;
        if( candidate->face.dir() != face.dir() ) {
            hint = _( "Vehicles need to face the same direction." );
            offset = point_zero;
        } else if( !offset.has_value() ) {
            hint = _( "Loaded vehicle can't be fit onto the flatbed." );
            offset = point_zero;
        }
        result.emplace( candidate, std::make_pair( offset.value(), hint ) );
    }
    return result;
}

std::set<std::string> vehicle::get_vehicles_available_to_unload() const
{
    std::set<std::string> res;

    for( const vpart_reference &flatbed_vpr : get_all_parts() ) {
        if( !flatbed_vpr.info().has_flag( "FLATBED" ) ||
            !flatbed_vpr.part().has_flag( vehicle_part::carrying_flag ) ) {
            continue;
        }
        for( const vehicle_part *vp : get_parts_at( flatbed_vpr.pos(), "", part_status_flag::any ) ) {
            if( !vp->has_flag( vehicle_part::carried_flag ) ) {
                continue;
            }
            if( vp->carried_stack.empty() ) {
                debugmsg( "Lost carried_stack info on %s at %s", vp->info().get_id().str(), vp->mount.to_string() );
                continue;
            }
            const vehicle_part::carried_part_data &cpd = vp->carried_stack.top();
            res.emplace( cpd.veh_name );
        }
    }

    return res;
}

void vehicle::build_flatbed_menu( veh_menu &menu ) const
{
    menu.desc_lines_hint = std::max( 1, menu.desc_lines_hint );
    bool has_flatbed_actions = false;
    const std::set<std::string> loaded_vehs = get_vehicles_available_to_unload();

    for( const std::string &unloadable : loaded_vehs ) {
        menu.add( string_format( _( "Unload the %s from the flatbed" ), unloadable ) )
        .hotkey_auto()
        .skip_locked_check()
        .on_submit( [this, unloadable] {
            flatbed_unloading_activity_actor unload_act( *this, unloadable );
            get_player_character().assign_activity( player_activity( unload_act ), false );
        } );

        has_flatbed_actions = true;
    }

    for( const auto &loadable : get_vehicles_available_to_load() ) {
        std::string desc = loadable.second.second;
        if( !loaded_vehs.empty() ) {
            desc = string_format( _( "A vehicle is already carried on the flatbed." ), loadable.first->name );
        }

        menu.add( string_format( _( "Load the %s on the flatbed" ), loadable.first->name ) )
        .enable( desc.empty() )
        .desc( desc )
        .hotkey_auto()
        .skip_locked_check()
        .on_submit( [this, loadable] {
            flatbed_loading_activity_actor load_act( *this, *loadable.first );
            get_player_character().assign_activity( player_activity( load_act ) );
        } );

        has_flatbed_actions = true;
    }

    if( !has_flatbed_actions ) {
        menu.add( _( "Flatbed is empty" ) )
        .desc( _( "Nothing available to load or unload on the flatbed is nearby." ) )
        .enable( false )
        .skip_locked_check();
    }
}

flatbed_loading_activity_actor::flatbed_loading_activity_actor( const vehicle &parent_vehicle,
        const vehicle &loaded_vehicle )
{
    parent_vehicle_pos = parent_vehicle.bub_part_pos( 0 );
    loaded_vehicle_pos = loaded_vehicle.bub_part_pos( 0 );
    loaded_vehicle_name = loaded_vehicle.name;
}

bool flatbed_loading_activity_actor::load_vehicle( bool check_only )
{
    map &here = get_map();
    const optional_vpart_position loader = here.veh_at( parent_vehicle_pos );
    const optional_vpart_position loaded = here.veh_at( loaded_vehicle_pos );
    if( !loader || !loaded ) {
        debugmsg( "flatbed or loaded vehicle not found" );
        return false;
    }
    vehicle &loader_veh = loader->vehicle();
    vehicle &loaded_veh = loaded->vehicle();

    const vpart_reference vpr( loader_veh, loader->part_index() );
    const auto candidates = loader_veh.get_vehicles_available_to_load();

    cata::optional<point> offset = cata::nullopt;
    for( const std::pair<vehicle *, std::pair<point, std::string>> &e : candidates ) {
        if( e.first == &loaded_veh ) {
            offset = e.second.first;
            break;
        }
    }
    if( !offset.has_value() ) {
        debugmsg( "Candidate has no available load position" );
        return false;
    }

    if( check_only ) {
        return true;
    }

    decltype( loader_veh.loot_zones ) new_zones;

    loader_veh.invalidate_towing( true );
    loader_veh.suspend_refresh();
    loaded_veh.unboard_all();
    for( const vpart_reference &vpr : loaded_veh.get_all_parts() ) {
        vehicle_part &old_pt = vpr.part();
        if( old_pt.removed || old_pt.is_fake ) {
            continue;
        }
        const point mount_pt = old_pt.mount - offset.value();
        const int flat_idx = loader_veh.part_with_feature( mount_pt, "FLATBED", true );
        const int part_idx = loader_veh.install_part( mount_pt, old_pt );

        vehicle_part &new_pt = loader_veh.part( part_idx );
        vehicle_part &flat_pt = loader_veh.part( flat_idx );

        new_pt.carried_stack
        .push( { tripoint( old_pt.mount, 0 ), old_pt.direction, loaded_veh.name, false } );

        new_pt.enabled = false;
        new_pt.set_flag( vehicle_part::carried_flag );
        if( loaded_veh.tracking_on ) {
            new_pt.set_flag( vehicle_part::tracked_flag );
        }

        flat_pt.set_flag( vehicle_part::carrying_flag );

        if( new_zones.find( new_pt.mount ) == new_zones.end() ) { // once per mount point
            const auto zones_on_point = loaded_veh.loot_zones.equal_range( old_pt.mount );
            for( auto it = zones_on_point.first; it != zones_on_point.second; ++it ) {
                new_zones.emplace( new_pt.mount, it->second );
            }
        }
    }

    for( std::pair<const point, zone_data> &new_zone : new_zones ) {
        zone_data data = new_zone.second;
        tripoint pos = tripoint_zero;//( new_zone.first, 0 );
        zone_data new_data( data.get_name(), data.get_type(), data.get_faction(), data.get_invert(),
                            data.get_enabled(), pos, pos, zone_options::create( data.get_type() ), false );
        zone_manager::get_manager().create_vehicle_loot_zone( loader_veh, new_zone.first, new_data );
    }

    here.destroy_vehicle( &loaded_veh );
    here.dirty_vehicle_list.insert( &loader_veh );
    loader_veh.zones_dirty = true;
    loader_veh.enable_refresh();
    here.set_transparency_cache_dirty( loader_veh.sm_pos.z );
    here.set_seen_cache_dirty( tripoint_zero );
    here.invalidate_map_cache( here.get_abs_sub().z() );
    here.rebuild_vehicle_level_caches();

    return true;
}

void flatbed_loading_activity_actor::start( player_activity &act, Character &who )
{
    act.moves_total = moves_total;
    act.moves_left = moves_total;
}

void flatbed_loading_activity_actor::do_turn( player_activity &act, Character &who )
{
    if( calendar::once_every( 30_seconds ) ) {
        if( !load_vehicle( /* check_only = */ true ) ) {
            who.add_msg_if_player( m_bad, _( "You can't get the %s on the flatbed." ), loaded_vehicle_name );
            act.set_to_null();
        }
    }
}

void flatbed_loading_activity_actor::finish( player_activity &act, Character &who )
{
    if( load_vehicle( /* check_only = */ false ) ) {
        who.add_msg_if_player( _( "You load the %1$s on the flatbed." ), loaded_vehicle_name );
    } else {
        who.add_msg_if_player( _( "You fail to load the %1$s on the flatbed." ), loaded_vehicle_name );
    }
    act.set_to_null();
}

void flatbed_loading_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "moves_total", moves_total );
    jsout.member( "parent_vehicle_pos", parent_vehicle_pos );
    jsout.member( "loaded_vehicle_pos", loaded_vehicle_pos );
    jsout.member( "loaded_vehicle_name", loaded_vehicle_name );
    jsout.end_object();
}

std::unique_ptr<activity_actor> flatbed_loading_activity_actor::deserialize( JsonValue &jsin )
{
    flatbed_loading_activity_actor actor;
    JsonObject data = jsin.get_object();
    data.read( "moves_total", actor.moves_total );
    data.read( "parent_vehicle_pos", actor.parent_vehicle_pos );
    data.read( "loaded_vehicle_pos", actor.loaded_vehicle_pos );
    data.read( "loaded_vehicle_name", actor.loaded_vehicle_name );
    return actor.clone();
}

units::angle normalize( units::angle a, const units::angle &mod = 360_degrees );

bool flatbed_unloading_activity_actor::unload_vehicle( Character &who, bool check_only )
{
    map &here = get_map();
    const optional_vpart_position ovp_carrier = here.veh_at( parent_vehicle_pos );
    if( !ovp_carrier ) {
        debugmsg( "flatbed unloading failed, carrier vehicle not found at %s",
                  parent_vehicle_pos.to_string() );
        return false;
    }
    vehicle &carrier = ovp_carrier->vehicle();
    std::vector<int> parts_to_unload;
    std::set<int> parts_flatbed;
    std::set<point> parts_mounts;
    cata::optional<point> carried_pivot_mount = cata::nullopt;
    int max_carried_forward = std::numeric_limits<int>::min();
    int min_flatbed_x = std::numeric_limits<int>::max();
    int max_flatbed_x = std::numeric_limits<int>::min();
    int min_flatbed_y = std::numeric_limits<int>::max();
    int max_flatbed_y = std::numeric_limits<int>::min();
    for( const vpart_reference &vpr : carrier.get_all_parts() ) {
        const vehicle_part &vp = vpr.part();
        const int flatbed_part = vpr.vehicle().part_with_feature( vp.mount, "FLATBED", false );
        if( flatbed_part == -1 ) {
            continue;
        }
        const point fbmount = vpr.vehicle().part( flatbed_part ).mount;
        min_flatbed_x = std::min( min_flatbed_x, fbmount.x );
        max_flatbed_x = std::max( max_flatbed_x, fbmount.x );
        min_flatbed_y = std::min( min_flatbed_y, fbmount.y );
        max_flatbed_y = std::max( max_flatbed_y, fbmount.y );
        if( !vp.has_flag( vehicle_part::carried_flag ) ) {
            continue;
        }
        if( vp.carried_name() == unloaded_vehicle_name ) {
            const vehicle_part::carried_part_data &cpd = vp.carried_stack.top();
            max_carried_forward = std::max( max_carried_forward, cpd.mount.x );
            parts_mounts.emplace( cpd.mount.xy() );
            parts_to_unload.emplace_back( static_cast<int>( vpr.part_index() ) );
            parts_flatbed.emplace( static_cast<int>( flatbed_part ) );
            if( cpd.mount == tripoint_zero ) {
                carried_pivot_mount = vpr.mount();
            }
        }
    }

    if( !carried_pivot_mount.has_value() ) {
        debugmsg( "didn't find carried pivot" );
        return false;
    }

    const bool can_float = carrier.can_float();
    const auto invalid_pos = [&here, &can_float]( const tripoint & p ) {
        return ( !can_float && here.has_flag_ter( ter_furn_flag::TFLAG_DEEP_WATER, p ) )
               || ( get_avatar().pos() == p )
               || here.veh_at( p )
               || here.impassable( p );
    };

    cata::optional<tripoint> pivot_pos = cata::nullopt;
    const point offset( min_flatbed_x - max_carried_forward - 1, carried_pivot_mount->y );
    for( const point &mount : parts_mounts ) {
        const tripoint pos = carrier.global_pos3() + carrier.coord_translate( mount + offset );
        if( invalid_pos( pos ) ) {
            return false;
        }
        if( mount == point_zero ) {
            pivot_pos = pos;
        }
    }

    if( !pivot_pos.has_value() ) {
        return false;
    }

    if( check_only ) {
        return true;
    }

    return carrier.remove_carried_vehicle(
               parts_to_unload,
               std::vector<int>( parts_flatbed.begin(), parts_flatbed.end() ),
               tripoint_bub_ms( pivot_pos.value() ) );
}

flatbed_unloading_activity_actor::flatbed_unloading_activity_actor( const vehicle &parent_vehicle,
        const std::string &unloaded_name )
{
    parent_vehicle_pos = parent_vehicle.bub_part_pos( 0 );
    unloaded_vehicle_name = unloaded_name;
}

void flatbed_unloading_activity_actor::start( player_activity &act, Character & )
{
    act.moves_total = moves_total;
    act.moves_left = moves_total;
}

void flatbed_unloading_activity_actor::do_turn( player_activity &act, Character &who )
{
    if( calendar::once_every( 30_seconds ) ) {
        if( !unload_vehicle( who, /* check_only = */ true ) ) {
            who.add_msg_if_player( _( "Can't unload %s from the flatbed; not enough space." ),
                                   unloaded_vehicle_name );
            act.set_to_null();
        }
    }
}

void flatbed_unloading_activity_actor::finish( player_activity &act, Character &who )
{
    if( !unload_vehicle( who, /* check_only = */ false ) ) {
        who.add_msg_if_player( _( "Can't unload %s from the flatbed; not enough space." ),
                               unloaded_vehicle_name );
    } else {
        who.add_msg_if_player( _( "You unload %s from the flatbed." ), unloaded_vehicle_name );
    }
    act.set_to_null();
}

void flatbed_unloading_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "moves_total", moves_total );
    jsout.member( "parent_vehicle_pos", parent_vehicle_pos );
    jsout.member( "unloaded_vehicle_name", unloaded_vehicle_name );
    jsout.end_object();
}

std::unique_ptr<activity_actor> flatbed_unloading_activity_actor::deserialize( JsonValue &jsin )
{
    flatbed_unloading_activity_actor actor;
    JsonObject data = jsin.get_object();
    data.read( "moves_total", actor.moves_total );
    data.read( "parent_vehicle_pos", actor.parent_vehicle_pos );
    data.read( "unloaded_vehicle_name", actor.unloaded_vehicle_name );
    return actor.clone();
}
