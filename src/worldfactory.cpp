#include "worldfactory.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <exception>
#include <iterator>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

#include "cata_utility.h"
#include "catacharset.h"
#include "char_validity_check.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "enums.h"
#include "filesystem.h"
#include "input.h"
#include "input_context.h"
#include "input_popup.h"
#include "json.h"
#include "json_loader.h"
#include "mod_manager.h"
#include "output.h"
#include "path_info.h"
#include "point.h"
#include "popup.h"
#include "sounds.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "text_snippets.h"
#include "translations.h"
#include "uilist.h"
#include "ui_manager.h"
#include "zzip.h"
#include "zzip_stack.h"

// single instance of world generator
std::unique_ptr<worldfactory> world_generator;

/**
  * Max utf-8 character worldname length.
  * 0 index is inclusive.
  */
static const int max_worldname_len = 32;

save_t::save_t( const std::string &name ): name( name ) {}

std::string save_t::decoded_name() const
{
    return name;
}

std::string save_t::base_path() const
{
    return base64_encode( name );
}

save_t save_t::from_save_id( const std::string &save_id )
{
    return save_t( save_id );
}

save_t save_t::from_base_path( const std::string &base_path )
{
    return save_t( base64_decode( base_path ) );
}

static std::string get_next_valid_worldname()
{
    return SNIPPET.expand( "<world_name>" );
}

WORLD::WORLD()
{
    world_name = get_next_valid_worldname();
    WORLD_OPTIONS = get_options().get_world_defaults();

    world_saves.clear();
    active_mod_order = world_generator->get_mod_manager().get_default_mods();
}

WORLD::WORLD( const std::string &name )
{
    world_name = name;
    WORLD_OPTIONS = get_options().get_world_defaults();

    world_saves.clear();
    active_mod_order = world_generator->get_mod_manager().get_default_mods();
}

void WORLD::COPY_WORLD( const WORLD *world_to_copy )
{
    world_name = world_to_copy->world_name + "_copy";
    WORLD_OPTIONS = world_to_copy->WORLD_OPTIONS;
    active_mod_order = world_to_copy->active_mod_order;
}

cata_path WORLD::folder_path() const
{
    return PATH_INFO::savedir_path() / world_name;
}

bool WORLD::save_exists( const save_t &name ) const
{
    return std::find( world_saves.begin(), world_saves.end(), name ) != world_saves.end();
}

void WORLD::add_save( const save_t &name )
{
    if( !save_exists( name ) ) {
        world_saves.push_back( name );
    }
}

worldfactory::worldfactory()
    : active_world( nullptr )
    , mman_ui( *mman )
{
    // prepare tab display order
    tabs.emplace_back(
    [this]( const catacurses::window & win, WORLD * w, bool b ) {
        return show_worldgen_tab_modselection( win, w, b );
    } );
    tabs.emplace_back(
    [this]( const catacurses::window & win, WORLD * w, bool b ) {
        return show_worldgen_tab_options( win, w, b );
    } );
}

worldfactory::~worldfactory() = default;

WORLD *worldfactory::add_world( std::unique_ptr<WORLD> retworld )
{
    if( !retworld->save() ) {
        return nullptr;
    }
    return ( all_worlds[ retworld->world_name ] = std::move( retworld ) ).get();
}

WORLD *worldfactory::make_new_world( const std::vector<mod_id> &mods )
{
    std::unique_ptr<WORLD> retworld = std::make_unique<WORLD>();
    retworld->active_mod_order = mods;
    retworld->create_timestamp();
    return add_world( std::move( retworld ) );
}

WORLD *worldfactory::make_new_world( const std::string &name, const std::vector<mod_id> &mods )
{
    if( !is_lexically_valid( std::filesystem::u8path( name ) ) ) {
        return nullptr;
    }
    std::unique_ptr<WORLD> retworld = std::make_unique<WORLD>( name );
    retworld->active_mod_order = mods;
    retworld->create_timestamp();
    return add_world( std::move( retworld ) );
}

WORLD *worldfactory::make_new_world( bool show_prompt, const std::string &world_to_copy )
{
    // World to return after generating
    std::unique_ptr<WORLD> retworld = std::make_unique<WORLD>();

    if( !world_to_copy.empty() ) {
        retworld->COPY_WORLD( world_generator->get_world( world_to_copy ) );
    }

    if( show_prompt ) {
        if( world_to_copy.empty() ) {
            if( show_worldgen_basic( retworld.get() ) < 0 ) {
                return nullptr;
            }
        } else if( show_worldgen_advanced( retworld.get() ) < 0 ) {
            return nullptr;
        }
    }

    for( mod_id checked_mod : retworld->active_mod_order ) {
        if( !mman_ui->confirm_mod_compatibility( checked_mod, retworld->active_mod_order ) ) {
            popup( _( "Unable to create new world, some active mods are incompatible." ) );
            return nullptr;
        }
    }

    retworld->create_timestamp();

    return add_world( std::move( retworld ) );
}

static std::optional<std::string> prompt_world_name( const std::string &title,
        const std::string &cur_worldname )
{
    string_input_popup_imgui popup( 50, cur_worldname );
    popup.set_max_input_length( max_worldname_len );
    popup.set_label( title );

    input_context ctxt( "STRING_INPUT" );
    popup.set_description( string_format(
                               _( "Press [<color_c_yellow>%s</color>] to randomize the world name." ),
                               ctxt.get_desc( "PICK_RANDOM_WORLDNAME", 1U ) ) );

    popup.add_callback( callback_input{ "PICK_RANDOM_WORLDNAME" }, [&popup]() {
        popup.set_text( get_next_valid_worldname() );
        return true;
    } );
    std::string message = popup.query();
    return message;
}

int worldfactory::show_worldgen_advanced( WORLD *world )
{
    // set up window
    catacurses::window wf_win;
    ui_adaptor ui;

    const auto init_windows = [&]( ui_adaptor & ui ) {
        const int iMinScreenWidth = std::max( FULL_SCREEN_WIDTH, TERMX / 2 );
        const int iOffsetX = TERMX > FULL_SCREEN_WIDTH ? ( TERMX - iMinScreenWidth ) / 2 : 0;
        wf_win = catacurses::newwin( TERMY, iMinScreenWidth, point( iOffsetX, 0 ) );
        ui.position_from_window( wf_win );
    };
    init_windows( ui );
    ui.on_screen_resize( init_windows );

    int curtab = 0;

    ui.on_redraw( [&]( const ui_adaptor & ) {
        draw_worldgen_tabs( wf_win, static_cast<size_t>( curtab ) );
        wnoutrefresh( wf_win );
    } );

    const size_t numtabs = tabs.size();
    bool done = false;
    while( !done ) {
        while( static_cast<size_t>( curtab ) < numtabs ) {
            ui_manager::redraw();
            curtab += tabs[curtab]( wf_win, world, true );
        }
        if( curtab >= 0 ) {
            std::optional<std::string> ret = prompt_world_name( _( "Choose a new name for this world." ),
                                             world->world_name );
            if( !ret.has_value() ) {
                // return to settings tab
                curtab = 1;
            } else if( ret.value().empty() ) {
                // no name entered
                if( query_yn( _( "World name is empty.  Randomize the name?" ) ) ) {
                    world->world_name = pick_random_name();
                }
            } else {
                // done, generate world
                world->world_name = ret.value();
                done = true;
            }
        } else if( curtab < 0 ) {
            break;
        }
    }
    return curtab;
}

WORLD *worldfactory::make_new_world( special_game_type special_type )
{
    std::string worldname;
    switch( special_type ) {
        case special_game_type::TUTORIAL:
            worldname = "TUTORIAL";
            break;
        default:
            return nullptr;
    }

    // Look through all worlds and see if a world named worldname already exists. If so, then just return it instead of
    // making a new world.
    if( has_world( worldname ) ) {
        return all_worlds[worldname].get();
    }

    std::unique_ptr<WORLD> special_world = std::make_unique<WORLD>();
    special_world->world_name = worldname;

    special_world->WORLD_OPTIONS["WORLD_END"].setValue( "delete" );

    special_world->create_timestamp();

    if( !special_world->save() ) {
        return nullptr;
    }

    return ( all_worlds[worldname] = std::move( special_world ) ).get();
}

void worldfactory::set_active_world( WORLD *world )
{
    world_generator->active_world = world;
    if( world ) {
        get_options().set_world_options( &world->WORLD_OPTIONS );
    } else {
        get_options().set_world_options( nullptr );
    }
}

bool WORLD::save() const
{
    if( !assure_dir_exist( folder_path() ) ) {
        debugmsg( "Unable to create or open world[%s] directory for saving", world_name );
        DebugLog( D_ERROR, DC_ALL ) << "Unable to create or open world[" << world_name <<
                                    "] directory for saving";
        return false;
    }

    if( !save_timestamp() ) {
        return false;
    }

    const cata_path savefile = folder_path() / PATH_INFO::worldoptions();
    const bool saved = write_to_file( savefile, [&]( std::ostream & fout ) {
        JsonOut jout( fout );

        jout.start_array();

        for( const auto &elem : WORLD_OPTIONS ) {
            // Skip hidden option because it is set by mod and should not be saved
            if( !elem.second.getDefaultText().empty() ) {
                jout.start_object();

                jout.member( "info", elem.second.getTooltip() );
                jout.member( "default", elem.second.getDefaultText( false ) );
                jout.member( "name", elem.first );
                jout.member( "value", elem.second.getValue( true ) );

                jout.end_object();
            }
        }

        jout.end_array();
    }, _( "world data" ) );
    if( !saved ) {
        return false;
    }

    world_generator->get_mod_manager().save_mods_list( this );
    return true;
}

void worldfactory::init()
{
    load_last_world_info();

    all_worlds.clear();

    // The validity of a world is determined by the existence of any
    // option files or the master save file.
    static const auto is_save_dir = []( const std::string & maybe_save_dir ) {
        return file_exist( maybe_save_dir + "/" + PATH_INFO::worldoptions() ) ||
               file_exist( maybe_save_dir + "/" + SAVE_MASTER );
    };

    const auto add_existing_world = [&]( const std::string & world_dir ) {
        // get the save files
        auto world_sav_files = get_files_from_path( SAVE_EXTENSION, world_dir, false );
        // split the save file names between the directory and the extension
        for( auto &world_sav_file : world_sav_files ) {
            const size_t start_of_file = world_dir.size() + 1;
            size_t save_index = world_sav_file.find( SAVE_EXTENSION, start_of_file );
            world_sav_file = world_sav_file.substr( start_of_file,
                                                    save_index - start_of_file );
        }

        // the directory name is the name of the world
        std::string worldname;
        size_t name_index = world_dir.find_last_of( "/\\" );
        worldname = world_dir.substr( name_index + 1 );

        // create and store the world
        all_worlds[worldname] = std::make_unique<WORLD>();
        // give the world a name
        all_worlds[worldname]->world_name = worldname;

        bool save = false;

        // load timestamp or create for legacy world
        if( !all_worlds[worldname]->load_timestamp() ) {
            all_worlds[worldname]->create_timestamp();
            save = true;
        }

        // add sav files
        for( auto &world_sav_file : world_sav_files ) {
            all_worlds[worldname]->world_saves.push_back( save_t::from_base_path( world_sav_file ) );
        }
        mman->load_mods_list( all_worlds[worldname].get() );

        // load options into the world
        if( !all_worlds[worldname]->load_options() ) {
            all_worlds[worldname]->WORLD_OPTIONS = get_options().get_world_defaults();
            all_worlds[worldname]->WORLD_OPTIONS["WORLD_END"].setValue( "delete" );
            save = true;
        }

        if( save ) {
            all_worlds[worldname]->save();
        }
    };

    // This returns files as well, but they are going to be discarded later as
    // we look for files *within* these dirs. If it's a file, there won't be
    // any files inside it and is_save_dir will return false.
    for( const std::string &dir : get_files_from_path( "", PATH_INFO::savedir(), false ) ) {
        if( !is_save_dir( dir ) ) {
            continue;
        }
        add_existing_world( dir );
    }
}

bool worldfactory::has_world( const std::string &name ) const
{
    return all_worlds.count( name ) > 0;
}

const std::map<std::string, std::unique_ptr<WORLD>> &worldfactory::get_all_worlds() const
{
    return all_worlds;
}

std::vector<std::string> worldfactory::all_worldnames() const
{
    std::vector<std::string> result;
    result.reserve( all_worlds.size() );
    for( const auto &elem : all_worlds ) {
        result.push_back( elem.first );
    }
    return result;
}

WORLD *worldfactory::pick_world( bool show_prompt, bool empty_only )
{
    std::vector<std::string> world_names = all_worldnames();

    // Filter out special worlds (TUTORIAL | DEFENSE) from world_names.
    for( std::vector<std::string>::iterator it = world_names.begin(); it != world_names.end(); ) {
        if( *it == "TUTORIAL" || *it == "DEFENSE" ||
            ( empty_only && !get_world( *it )->world_saves.empty() ) ) {
            it = world_names.erase( it );
        } else {
            ++it;
        }
    }
    // If there is only one world to pick from, autoreturn it.
    if( world_names.size() == 1 ) {
        return get_world( world_names[0] );
    }
    // If there are no worlds to pick from, immediately try to make one.
    else if( world_names.empty() ) {
        return make_new_world( show_prompt );
    }
    // If we're skipping prompts, return the world with 0 save if there is one
    else if( !show_prompt ) {
        for( const std::string &name : world_names ) {
            if( get_world( name )->world_saves.empty() ) {
                return get_world( name );
            }
        }
        // if there isn't any, adhere to old logic: return the alphabetically first one
        return get_world( world_names[0] );
    }

    const int iTooltipHeight = 3;
    int iContentHeight = 0;
    int iMinScreenWidth = 0;
    size_t num_pages = 1;

    std::set<int> mapLines;
    mapLines.insert( 3 );

    std::map<int, std::vector<std::string> > world_pages;
    std::map<int, inclusive_rectangle<point>> button_map;
    point world_list_top_left;
    int world_list_width = 0;
    int sel = 0;
    size_t selpage = 0;

    catacurses::window w_worlds_border;
    catacurses::window w_worlds_tooltip;
    catacurses::window w_worlds_header;
    catacurses::window w_worlds;

    ui_adaptor ui;

    const auto on_move = []( bool is_error ) {
        sfx::play_variant_sound( is_error ? "menu_error" : "menu_move", "default", 100 );
    };

    const auto init_windows = [&]( ui_adaptor & ui ) {
        iContentHeight = TERMY - 3 - iTooltipHeight;
        iMinScreenWidth = std::max( FULL_SCREEN_WIDTH, TERMX / 2 );
        const int iOffsetX = TERMX > FULL_SCREEN_WIDTH ? ( TERMX - iMinScreenWidth ) / 2 : 0;
        num_pages = world_names.size() / iContentHeight + 1; // at least 1 page

        world_pages.clear();
        size_t worldnum = 0;
        for( size_t i = 0; i < num_pages; ++i ) {
            for( int j = 0; j < iContentHeight && worldnum < world_names.size(); ++j ) {
                world_pages[i].push_back( world_names[ worldnum++ ] );
            }
        }

        w_worlds_border  = catacurses::newwin( TERMY, iMinScreenWidth,
                                               point( iOffsetX, 0 ) );
        w_worlds_tooltip = catacurses::newwin( iTooltipHeight, iMinScreenWidth - 2,
                                               point( 1 + iOffsetX, 1 ) );
        w_worlds_header  = catacurses::newwin( 1, iMinScreenWidth - 2,
                                               point( 1 + iOffsetX, 1 + iTooltipHeight ) );
        w_worlds         = catacurses::newwin( iContentHeight, iMinScreenWidth - 2,
                                               point( 1 + iOffsetX, iTooltipHeight + 2 ) );

        world_list_top_left = point( getbegx( w_worlds ), getbegy( w_worlds ) );
        world_list_width = iMinScreenWidth - 2;

        ui.position_from_window( w_worlds_border );
    };
    init_windows( ui );
    ui.on_screen_resize( init_windows );

    ui.on_redraw( [&]( const ui_adaptor & ) {
        button_map.clear();
        draw_border( w_worlds_border, BORDER_COLOR, _( "World selection" ) );
        wattron( w_worlds_border, BORDER_COLOR );
        mvwaddch( w_worlds_border, point( 0, 4 ), LINE_XXXO ); // |-
        mvwaddch( w_worlds_border, point( iMinScreenWidth - 1, 4 ), LINE_XOXX ); // -|

        for( const int &mapLine : mapLines ) {
            mvwaddch( w_worlds_border, point( mapLine + 1, TERMY - 1 ), LINE_XXOX ); // _|_
        }
        wattroff( w_worlds_border, BORDER_COLOR );

        wnoutrefresh( w_worlds_border );

        wattron( w_worlds_header, BORDER_COLOR );
        mvwhline( w_worlds_header, point::zero, LINE_OXOX, getmaxx( w_worlds_border ) );
        for( const int &mapLine : mapLines ) {
            mvwaddch( w_worlds_header, point( mapLine, 0 ), LINE_OXXX ); // ^|^
        }
        wattroff( w_worlds_header, BORDER_COLOR );

        wnoutrefresh( w_worlds_header );

        //Clear the lines
        mvwrectf( w_worlds, point::zero, c_black, ' ', getmaxx( w_worlds ), iContentHeight );
        for( const int &mapLine : mapLines ) {
            mvwvline( w_worlds, point( mapLine, 1 ), BORDER_COLOR, LINE_XOXO, iContentHeight - 2 );
        }
        mvwrectf( w_worlds_tooltip, point::zero, c_black, ' ', getmaxx( w_worlds ), iTooltipHeight );

        //Draw World Names
        for( size_t i = 0; i < world_pages[selpage].size(); ++i ) {
            const bool sel_this = static_cast<int>( i ) == sel;
            inclusive_rectangle<point> btn( world_list_top_left + point( 4, i ),
                                            world_list_top_left + point( world_list_width - 1, i ) );
            button_map.emplace( static_cast<int>( i ), btn );

            mvwprintz( w_worlds, point( 0, static_cast<int>( i ) ), c_white, "%d", i + 1 );
            wmove( w_worlds, point( 4, static_cast<int>( i ) ) );

            std::string world_name = ( world_pages[selpage] )[i];
            size_t saves_num = get_world( world_name )->world_saves.size();

            if( sel_this ) {
                wprintz( w_worlds, hilite( c_yellow ), "» " );
            } else {
                wprintz( w_worlds, c_yellow, "  " );
            }

            const std::string txt = string_format( "%s (%lu)", world_name, saves_num );
            const int remaining = world_list_width - ( utf8_width( txt, true ) + 6 );
            wprintz( w_worlds, sel_this ? hilite( c_white ) : c_white, txt );
            if( sel_this && remaining > 0 ) {
                wprintz( w_worlds, hilite( c_white ), std::string( remaining, ' ' ) );
            }
        }

        //Draw Tabs
        wmove( w_worlds_header, point( 7, 0 ) );

        for( size_t i = 0; i < num_pages; ++i ) {
            //skip empty pages
            if( !world_pages[i].empty() ) {
                nc_color tabcolor = ( selpage == i ) ? hilite( c_white ) : c_white;
                wprintz( w_worlds_header, c_white, "[" );
                wprintz( w_worlds_header, tabcolor, _( "Page %lu" ), i + 1 );
                wprintz( w_worlds_header, c_white, "]" );
                wputch( w_worlds_header, BORDER_COLOR, LINE_OXOX );
            }
        }

        wnoutrefresh( w_worlds_header );

        fold_and_print( w_worlds_tooltip, point::zero, 78, c_white, _( "Pick a world to enter game" ) );
        wnoutrefresh( w_worlds_tooltip );

        wnoutrefresh( w_worlds );
    } );

    input_context ctxt( "PICK_WORLD_DIALOG" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "CONFIRM" );
    // for mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );

    while( true ) {
        ui_manager::redraw();

        std::string action = ctxt.handle_input();
        const size_t recmax = world_pages[selpage].size();
        const size_t scroll_rate = recmax > 20 ? 10 : 3;

        // handle mouse click
        if( action == "SELECT" || action == "MOUSE_MOVE" ) {
            std::optional<point> coord = ctxt.get_coordinates_text( catacurses::stdscr );
            if( !!coord ) {
                int cnt = run_for_point_in<int, point>( button_map, *coord,
                [&sel, &on_move]( const std::pair<int, inclusive_rectangle<point>> &p ) {
                    if( sel != p.first ) {
                        on_move( false );
                        sel = p.first;
                    }
                } );
                if( cnt > 0 ) {
                    if( action == "SELECT" ) {
                        action = "CONFIRM";
                    }
                    ui_manager::redraw();
                }
            }
        }

        if( action == "QUIT" ) {
            break;
        } else if( !world_pages[selpage].empty() &&
                   navigate_ui_list( action, sel, scroll_rate, recmax, true ) ) {
            on_move( recmax < 2 );
        } else if( action == "NEXT_TAB" ) {
            sel = 0;

            do {
                //skip empty pages
                selpage++;
                if( selpage >= world_pages.size() ) {
                    selpage = 0;
                }
            } while( world_pages[selpage].empty() );
        } else if( action == "PREV_TAB" ) {
            sel = 0;
            do {
                //skip empty pages
                if( selpage != 0 ) {
                    selpage--;
                } else {
                    selpage = world_pages.size() - 1;
                }
            } while( world_pages[selpage].empty() );
        } else if( action == "CONFIRM" ) {
            return get_world( world_pages[selpage][sel] );
        }
    }

    return nullptr;
}

void worldfactory::remove_world( const std::string &worldname )
{
    auto it = all_worlds.find( worldname );
    if( it != all_worlds.end() ) {
        WORLD *wptr = it->second.get();
        if( active_world == wptr ) {
            get_options().set_world_options( nullptr );
            active_world = nullptr;
        }
        all_worlds.erase( it );
    }
}

void worldfactory::load_last_world_info()
{
    cata_path lastworld_path = PATH_INFO::lastworld();
    if( !file_exist( lastworld_path ) ) {
        return;
    }

    try {
        JsonValue jsin = json_loader::from_path( lastworld_path );
        JsonObject data = jsin.get_object();
        last_world_name = data.get_string( "world_name" );
        last_character_name = data.get_string( "character_name" );
    } catch( std::exception const &e ) {
        DebugLog( D_INFO, DC_ALL ) <<  e.what();
        last_world_name = std::string{};
        last_character_name = std::string{};
    }
}

void worldfactory::save_last_world_info() const
{
    write_to_file( PATH_INFO::lastworld(), [&]( std::ostream & file ) {
        JsonOut jsout( file, true );
        jsout.start_object();
        jsout.member( "world_name", last_world_name );
        jsout.member( "character_name", last_character_name );
        jsout.end_object();
    }, _( "last world info" ) );
}

std::string worldfactory::pick_random_name()
{
    // TODO: add some random worldname parameters to name generator
    return get_next_valid_worldname();
}

int worldfactory::show_worldgen_tab_options( const catacurses::window &, WORLD *world,
        bool with_tabs )
{
    get_options().set_world_options( &world->WORLD_OPTIONS );
    const std::string action = get_options().show( false, true, with_tabs );
    get_options().set_world_options( nullptr );
    if( action == "PREV_TAB" ) {
        return -1;

    } else if( action == "NEXT_TAB" ) {
        return 1;

    } else if( action == "QUIT" ) {
        return -999;
    }

    return 0;
}

std::map<int, inclusive_rectangle<point>> worldfactory::draw_mod_list( const catacurses::window &w,
                                       int &start, size_t cursor, const std::vector<mod_id> &mods,
                                       bool is_active_list, const std::string &text_if_empty,
                                       const catacurses::window &w_shift, bool recalc_start,
                                       const std::vector<mod_id> &potential_conflicts )
{
    werase( w );
    werase( w_shift );

    std::map<int, inclusive_rectangle<point>> ent_map;

    const int iMaxRows = getmaxy( w );
    size_t iModNum = mods.size();
    size_t iActive = cursor;
    bool first_line_is_category = false;

    if( mods.empty() ) {
        center_print( w, 0, c_red, text_if_empty );
    } else {
        int iCatSortNum = 0;
        std::string sLastCategoryName;
        std::map<int, std::string> mSortCategory;
        mSortCategory[0] = sLastCategoryName;

        for( size_t i = 0; i < mods.size(); ++i ) {
            std::string category_name = _( "MISSING MODS" );
            if( mods[i].is_valid() ) {
                category_name = mods[i]->obsolete ? _( "OBSOLETE MODS" ) : mods[i]->category.second.translated();
            }
            if( sLastCategoryName != category_name ) {
                sLastCategoryName = category_name;
                mSortCategory[ i + iCatSortNum++ ] = sLastCategoryName;
                iModNum++;
                if( i == 0 ) {
                    first_line_is_category = true;
                }
            }
        }

        const int wwidth = getmaxx( w ) - 1 - 3; // border (1) + ">> " (3)

        unsigned int iNum = 0;
        bool bKeepIter = false;

        for( size_t i = 0; i <= iActive; i++ ) {
            if( !mSortCategory[i].empty() ) {
                iActive++;
            }
        }

        if( recalc_start ) {
            calcStartPos( start, iActive, iMaxRows, iModNum );
        }

        for( int i = 0; i < start; i++ ) {
            if( !mSortCategory[i].empty() ) {
                iNum++;
            }
        }

        int larger = ( iMaxRows > static_cast<int>( iModNum ) ) ? static_cast<int>( iModNum ) : iMaxRows;
        for( auto iter = mods.begin(); iter != mods.end(); ) {
            if( iNum >= static_cast<size_t>( start ) && iNum < static_cast<size_t>( start ) + larger ) {
                if( !mSortCategory[iNum].empty() ) {
                    bKeepIter = true;
                    trim_and_print( w, point( 1, iNum - start ), wwidth, c_magenta, mSortCategory[iNum] );

                } else {
                    if( iNum == iActive ) {
                        //mvwprintw( w, iNum - start + iCatSortOffset, 1, "   " );
                        if( is_active_list ) {
                            mvwprintz( w, point( 1, iNum - start ), c_yellow, ">> " );
                        } else {
                            mvwprintz( w, point( 1, iNum - start ), c_blue, ">> " );
                        }
                    }

                    const mod_id &mod_entry_id = *iter;
                    std::string mod_entry_name;
                    nc_color mod_entry_color = c_white;
                    if( mod_entry_id.is_valid() ) {
                        const MOD_INFORMATION &mod = *mod_entry_id;
                        mod_entry_name = mod.name();
                        if( mod.obsolete ) {
                            mod_entry_color = c_dark_gray;
                        }
                    } else {
                        mod_entry_color = c_light_red;
                        mod_entry_name = _( "N/A" );

                    }
                    mod_entry_name += string_format( _( " [%s]" ), mod_entry_id.str() );
                    if( !mman_ui->confirm_mod_compatibility( mod_entry_id, potential_conflicts ) ) {
                        mod_entry_name += _( " --- INCOMPATIBLE!" );
                        mod_entry_color = c_pink;
                    }
                    trim_and_print( w, point( 4, iNum - start ), wwidth, mod_entry_color, mod_entry_name );
                    ent_map.emplace( static_cast<int>( std::distance( mods.begin(), iter ) ),
                                     inclusive_rectangle<point>( point( 1, iNum - start ), point( 3 + wwidth, iNum - start ) ) );

                    if( w_shift ) {
                        // get shift information for the active item
                        std::string shift_display;
                        const size_t iPos = std::distance( mods.begin(), iter );

                        if( mman_ui->can_shift_up( iPos, mods ) ) {
                            shift_display += "<color_blue>+</color> ";
                        } else {
                            shift_display += "<color_dark_gray>+</color> ";
                        }

                        if( mman_ui->can_shift_down( iPos, mods ) ) {
                            shift_display += "<color_blue>-</color>";
                        } else {
                            shift_display += "<color_dark_gray>-</color>";
                        }

                        trim_and_print( w_shift, point( 1, 2 + iNum - start ), 3, c_white, shift_display );
                    }
                }
            }

            if( bKeepIter ) {
                bKeepIter = false;
            } else {
                ++iter;
            }

            iNum++;
        }
    }

    // Ensure that the scrollbar starts at zero position
    if( first_line_is_category && iActive == 1 ) {
        draw_scrollbar( w, 0, iMaxRows, static_cast<int>( iModNum ), point::zero );
    } else {
        draw_scrollbar( w, static_cast<int>( iActive ), iMaxRows, static_cast<int>( iModNum ),
                        point::zero );
    }

    wnoutrefresh( w );
    wnoutrefresh( w_shift );

    return ent_map;
}

void worldfactory::show_active_world_mods( const std::vector<mod_id> &world_mods )
{
    ui_adaptor ui;
    catacurses::window w_border;
    catacurses::window w_mods;
    std::map<int, inclusive_rectangle<point>> ent_map;
    bool recalc_start = false;

    const auto init_windows = [&]( ui_adaptor & ui ) {
        recalc_start = true;
        const int iMinScreenWidth = std::max( FULL_SCREEN_WIDTH, TERMX / 2 );
        const int iOffsetX = TERMX > FULL_SCREEN_WIDTH ? ( TERMX - iMinScreenWidth ) / 2 : 0;

        w_border = catacurses::newwin( TERMY - 11, iMinScreenWidth, point( iOffsetX, 4 ) );
        w_mods   = catacurses::newwin( TERMY - 13, iMinScreenWidth - 1, point( iOffsetX, 5 ) );

        ui.position_from_window( w_border );
    };
    init_windows( ui );
    ui.on_screen_resize( init_windows );

    int start = 0;
    int cursor = 0;
    const size_t num_mods = world_mods.size();

    input_context ctxt( "DEFAULT" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    // for mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SEC_SELECT" );

    ui.on_redraw( [&]( const ui_adaptor & ) {
        draw_border( w_border, BORDER_COLOR, _( "Active world mods" ) );
        wnoutrefresh( w_border );

        ent_map = draw_mod_list( w_mods, start, static_cast<size_t>( cursor ), world_mods,
                                 true, _( "--NO ACTIVE MODS--" ), catacurses::window(), recalc_start );
        wnoutrefresh( w_mods );
    } );

    while( true ) {
        ui_manager::redraw();

        const std::string action = ctxt.handle_input();
        const int recmax = static_cast<int>( num_mods );
        const int scroll_rate = recmax > 20 ? 10 : 3;

        if( !world_mods.empty() && action == "MOUSE_MOVE" ) {
            std::optional<point> coord = ctxt.get_coordinates_text( w_mods );
            if( !!coord ) {
                run_for_point_in<int, point>( ent_map, *coord,
                [&cursor]( const std::pair<int, inclusive_rectangle<point>> &p ) {
                    cursor = p.first;
                } );
            }
        }

        if( navigate_ui_list( action, cursor, scroll_rate, recmax, true ) ) {
            recalc_start = true;
        } else if( action == "QUIT" || action == "CONFIRM" ||
                   action == "SELECT" || action == "SEC_SELECT" ) {
            break;
        }
    }
}

int worldfactory::show_worldgen_tab_modselection( const catacurses::window &win, WORLD *world,
        bool with_tabs )
{
    // Use active_mod_order of the world,
    // saves us from writing 'world->active_mod_order' all the time.
    std::vector<mod_id> &active_mod_order = world->active_mod_order;
    {
        std::vector<mod_id> tmp_mod_order;
        // clear active_mod_order and re-add all the mods, his ensures
        // that changes (like changing dependencies) get updated
        tmp_mod_order.swap( active_mod_order );
        for( auto &elem : tmp_mod_order ) {
            mman_ui->try_add( elem, active_mod_order );
        }
    }

    input_context ctxt( "MODMANAGER_DIALOG" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "LEFT", to_translation( "Switch to other list" ) );
    ctxt.register_action( "RIGHT", to_translation( "Switch to other list" ) );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "NEXT_CATEGORY_TAB" );
    ctxt.register_action( "PREV_CATEGORY_TAB" );
    if( with_tabs ) {
        ctxt.register_action( "NEXT_TAB" );
        ctxt.register_action( "PREV_TAB" );
    }
    ctxt.register_action( "CONFIRM", to_translation( "Activate / deactivate mod" ) );
    ctxt.register_action( "MOVE_MOD_UP" );
    ctxt.register_action( "MOVE_MOD_DOWN" );
    ctxt.register_action( "SAVE_DEFAULT_MODS" );
    ctxt.register_action( "VIEW_MOD_DESCRIPTION" );
    ctxt.register_action( "FILTER" );
    // for mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );

    point filter_pos;
    int filter_view_len = 0;
    std::string current_filter = "init me!";
    std::unique_ptr<string_input_popup> fpopup;
    bool recalc_start = false;

    catacurses::window w_header1;
    catacurses::window w_header2;
    catacurses::window w_shift;
    catacurses::window w_list;
    catacurses::window w_active;
    catacurses::window w_description;
    std::vector<catacurses::window> header_windows;

    ui_adaptor ui;

    const auto init_windows = [&]( ui_adaptor & ui ) {
        recalc_start = true;
        const int iMinScreenWidth = std::max( FULL_SCREEN_WIDTH, TERMX / 2 );
        const int iOffsetX = TERMX > FULL_SCREEN_WIDTH ? ( TERMX - iMinScreenWidth ) / 2 : 0;

        w_header1     = catacurses::newwin( 1, iMinScreenWidth / 2 - 5,
                                            point( 1 + iOffsetX, 3 ) );
        w_header2     = catacurses::newwin( 1, iMinScreenWidth / 2 - 4,
                                            point( iMinScreenWidth / 2 + 3 + iOffsetX, 3 ) );
        w_shift       = catacurses::newwin( TERMY - 14, 5,
                                            point( iMinScreenWidth / 2 - 3 + iOffsetX, 3 ) );
        w_list        = catacurses::newwin( TERMY - 16, iMinScreenWidth / 2 - 4,
                                            point( iOffsetX, 5 ) );
        w_active      = catacurses::newwin( TERMY - 16, iMinScreenWidth / 2 - 4,
                                            point( iMinScreenWidth / 2 + 2 + iOffsetX, 5 ) );
        w_description = catacurses::newwin( 5, iMinScreenWidth - 4,
                                            point( 1 + iOffsetX, TERMY - 6 ) );

        header_windows.clear();
        header_windows.push_back( w_header1 );
        header_windows.push_back( w_header2 );

        // Specify where the popup's string would be printed
        filter_pos = point( 2, TERMY - 11 );
        filter_view_len = iMinScreenWidth / 2 - 11;
        if( fpopup ) {
            point inner_pos = filter_pos + point( 2, 0 );
            fpopup->window( win, inner_pos, inner_pos.x + filter_view_len );
        }

        ui.position_from_window( win );
    };
    init_windows( ui );
    ui.on_screen_resize( init_windows );

    std::vector<std::string> headers;
    headers.emplace_back( _( "Mod List" ) );
    headers.emplace_back( _( "Mod Load Order" ) );

    size_t active_header = 0;
    std::array<int, 2> startsel = {0, 0};
    std::array<size_t, 2> cursel = {0, 0};
    size_t iCurrentTab = 0;
    size_t sel_top_tab = 0;
    std::vector<mod_id> current_tab_mods;
    std::map<int, inclusive_rectangle<point>> inact_mod_map;
    std::map<int, inclusive_rectangle<point>> act_mod_map;
    std::map<int, inclusive_rectangle<point>> mod_tab_map;
    std::map<size_t, inclusive_rectangle<point>> top_tab_map;

    struct mod_tab {
        std::string id;
        std::vector<mod_id> mods;
        std::vector<mod_id> mods_unfiltered;

        explicit mod_tab( std::string id, std::vector<mod_id> mods, std::vector<mod_id> mods_unfiltered ) :
            id( std::move( id ) ), mods( std::move( mods ) ), mods_unfiltered( std::move( mods_unfiltered ) ) {}
    };
    std::vector<mod_tab> all_tabs;

    for( const std::pair<std::string, translation> &tab : get_mod_list_tabs() ) {
        all_tabs.emplace_back( tab.first, std::vector<mod_id> {}, std::vector<mod_id> {} );
    }

    const std::map<std::string, std::string> &cat_tab_map = get_mod_list_cat_tab();
    for( const mod_id &mod : mman->get_usable_mods() ) {
        int cat_idx = mod->category.first;
        const std::string &cat_id = get_mod_list_categories()[cat_idx].first;

        std::string dest_tab = "tab_default";
        const auto iter = cat_tab_map.find( cat_id );
        if( iter != cat_tab_map.end() ) {
            dest_tab = iter->second;
        }

        for( mod_tab &tab : all_tabs ) {
            if( tab.id == dest_tab ) {
                tab.mods_unfiltered.push_back( mod );
                break;
            }
        }
    }

    // Helper function for determining the currently selected mod
    const auto get_selected_mod = [&]() -> const MOD_INFORMATION* {
        const std::vector<mod_id> &current_tab_mods = all_tabs[iCurrentTab].mods;
        if( current_tab_mods.empty() )
        {
            return nullptr;
        } else if( active_header == 0 )
        {
            if( !current_tab_mods.empty() ) {
                return &current_tab_mods[cursel[0]].obj();
            }
            return nullptr;
        } else if( !active_mod_order.empty() )
        {
            return &active_mod_order[cursel[1]].obj();
        }
        return nullptr;
    };

    // Helper function for applying filter to mod tabs
    const auto apply_filter = [&]( const std::string & filter_str ) {
        if( filter_str == current_filter ) {
            return;
        }
        const MOD_INFORMATION *selected_mod = nullptr;
        if( active_header == 0 ) {
            selected_mod = get_selected_mod();
        }
        for( mod_tab &tab : all_tabs ) {
            if( filter_str.empty() ) {
                tab.mods = tab.mods_unfiltered;
            } else {
                tab.mods.clear();
                for( const mod_id &mod : tab.mods_unfiltered ) {
                    std::string name = ( *mod ).name();
                    if( lcmatch( name, filter_str ) ) {
                        tab.mods.push_back( mod );
                    }
                }
            }
        }
        startsel[0] = 0;
        cursel[0] = 0;
        // Try to restore cursor position
        const std::vector<mod_id> &curr_tab = all_tabs[iCurrentTab].mods;
        for( size_t i = 0; i < curr_tab.size(); i++ ) {
            if( &*curr_tab[i] == selected_mod ) {
                cursel[0] = i;
                break;
            }
        }
        current_filter = filter_str;
    };
    apply_filter( "" );

    ui.on_redraw( [&]( const ui_adaptor & ) {
        mod_tab_map.clear();
        if( with_tabs ) {
            top_tab_map = draw_worldgen_tabs( win, sel_top_tab );
        } else {
            werase( win );
            draw_border_below_tabs( win );
            wattron( win, c_light_gray );
            mvwaddch( win, point( 0, 2 ), LINE_OXXO ); // .-
            mvwhline( win, point( 1, 2 ), LINE_OXOX, getmaxx( win ) - 2 ); // -
            mvwaddch( win, point( getmaxx( win ) - 1, 2 ), LINE_OOXX ); // -.
            wattroff( win, c_light_gray );
        }
        draw_modselection_borders( win, ctxt );

        // Redraw headers
        for( size_t i = 0; i < headers.size(); ++i ) {
            werase( header_windows[i] );
            const int header_x = ( getmaxx( header_windows[i] ) - utf8_width( headers[i] ) ) / 2;
            mvwprintz( header_windows[i], point( header_x, 0 ), c_cyan, headers[i] );

            if( active_header == i ) {
                mvwputch( header_windows[i], point( header_x - 3, 0 ), c_red, '<' );
                mvwputch( header_windows[i], point( header_x + utf8_width( headers[i] ) + 2, 0 ),
                          c_red, '>' );
            }
            wnoutrefresh( header_windows[i] );
        }

        // Redraw description
        werase( w_description );

        if( const MOD_INFORMATION *selmod = get_selected_mod() ) {
            // NOLINTNEXTLINE(cata-use-named-point-constants)
            int num_lines = fold_and_print( w_description, point( 1, 0 ),
                                            getmaxx( w_description ) - 1,
                                            c_white, mman_ui->get_information( selmod ) );
            int window_height = catacurses::getmaxy( w_description );
            int window_width = catacurses::getmaxx( w_description );
            if( num_lines > window_height ) {
                // The description didn't fit in the window, so provide a
                // hint for how to see the whole thing
                std::string message = string_format( _( "…%s = View full description " ),
                                                     ctxt.get_desc( "VIEW_MOD_DESCRIPTION" ) );
                nc_color color = c_green;
                print_colored_text( w_description, point( window_width - utf8_width( message ), window_height - 1 ),
                                    color, color, message );
            }
        }

        // Draw tab names
        int xpos = 0;
        wmove( win, point( 2, 4 ) );
        for( size_t i = 0; i < get_mod_list_tabs().size(); i++ ) {
            wprintz( win, c_white, "[" );
            wprintz( win, ( iCurrentTab == i ) ? hilite( c_light_green ) : c_light_green,
                     "%s", get_mod_list_tabs()[i].second );
            wprintz( win, c_white, "]" );
            wputch( win, BORDER_COLOR, LINE_OXOX );
            point tabpos( point( 2 + ++xpos, 4 ) );
            int tabwidth = utf8_width( get_mod_list_tabs()[i].second.translated(), true );
            mod_tab_map.emplace( static_cast<int>( i ),
                                 inclusive_rectangle<point>( tabpos, tabpos + point( tabwidth, 0 ) ) );
            xpos += tabwidth + 2;
        }

        // Draw filter
        if( fpopup ) {
            mvwprintz( win, filter_pos, c_cyan, "< " );
            mvwprintz( win, filter_pos + point( filter_view_len + 2, 0 ), c_cyan, " >" );
            // This call makes popup draw its string at position specified on popup initialization
            fpopup->query_string( /*loop=*/false, /*draw_only=*/true );
        } else {
            mvwprintz( win, filter_pos, c_light_gray, "< " );
            const char *help = current_filter.empty() ? _( "[%s] Filter" ) : _( "[%s] Filter: " );
            wprintz( win, c_light_gray, help, ctxt.get_desc( "FILTER" ) );
            wprintz( win, c_white, current_filter );
            wprintz( win, c_light_gray, " >" );
        }

        wnoutrefresh( w_description );
        wnoutrefresh( win );

        // Draw selected tab
        const mod_tab &current_tab = all_tabs[iCurrentTab];
        const char *msg = current_tab.mods_unfiltered.empty() ?
                          _( "--NO AVAILABLE MODS--" ) : _( "--NO RESULTS FOUND--" );
        inact_mod_map = draw_mod_list( w_list, startsel[0], cursel[0], current_tab.mods,
                                       active_header == 0, msg, catacurses::window(), recalc_start, active_mod_order );

        // Draw active mods
        act_mod_map = draw_mod_list( w_active, startsel[1], cursel[1], active_mod_order,
                                     active_header == 1, _( "--NO ACTIVE MODS--" ), w_shift, recalc_start );
    } );

    const auto set_filter = [&]() {
        fpopup = std::make_unique<string_input_popup>();
        fpopup->max_length( 256 );
        // current_filter is modified by apply_filter(), we have to copy the value
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
        const std::string old_filter = current_filter;
        fpopup->text( current_filter );

        // On next redraw, call resize callback which will configure how popup is rendered
        ui.mark_resize();

        for( ;; ) {
            ui_manager::redraw();
            fpopup->query_string( /*loop=*/false );

            if( fpopup->canceled() ) {
                apply_filter( old_filter );
                break;
            } else if( fpopup->confirmed() ) {
                break;
            } else {
                apply_filter( fpopup->text() );
            }
        }

        fpopup.reset();
    };

    int tab_output = 0;
    while( tab_output == 0 ) {
        ui_manager::redraw();

        recalc_start = false;

        std::string action = ctxt.handle_input();
        size_t recmax = active_header == 0 ? static_cast<int>( all_tabs[iCurrentTab].mods.size() ) :
                        static_cast<int>( active_mod_order.size() );
        size_t scroll_rate = recmax > 20 ? 10 : 3;

        // Mouse selection
        if( action == "MOUSE_MOVE" || action == "SELECT" ) {
            bool found_opt = false;
            sel_top_tab = 0;
            std::optional<point> coord = ctxt.get_coordinates_text( win );
            if( !!coord ) {
                // Mod tabs
                bool new_val = false;
                found_opt = run_for_point_in<int, point>( mod_tab_map, *coord,
                [&iCurrentTab, &new_val]( const std::pair<int, inclusive_rectangle<point>> &p ) {
                    if( static_cast<int>( iCurrentTab ) != p.first ) {
                        new_val = true;
                        iCurrentTab = clamp<int>( p.first, 0, get_mod_list_tabs().size() - 1 );
                    }
                } ) > 0;
                if( new_val ) {
                    active_header = 0;
                    startsel[0] = 0;
                    cursel[0] = 0;
                    recalc_start = true;
                }
            }
            if( !found_opt && !!coord && with_tabs ) {
                // Top tabs
                found_opt = run_for_point_in<size_t, point>( top_tab_map, *coord,
                [&sel_top_tab]( const std::pair<size_t, inclusive_rectangle<point>> &p ) {
                    sel_top_tab = p.first;
                } ) > 0;
                if( found_opt ) {
                    if( action == "SELECT" ) {
                        tab_output = sel_top_tab;
                    }
                }
            }
            if( !found_opt ) {
                // Inactive mod list
                coord = ctxt.get_coordinates_text( w_list );
                if( !!coord ) {
                    found_opt = run_for_point_in<int, point>( inact_mod_map, *coord,
                    [&cursel]( const std::pair<int, inclusive_rectangle<point>> &p ) {
                        cursel[0] = p.first;
                    } );
                }
                if( found_opt ) {
                    active_header = 0;
                    if( action == "SELECT" ) {
                        action = "CONFIRM";
                    }
                }
            }
            if( !found_opt ) {
                // Active mod list
                coord = ctxt.get_coordinates_text( w_active );
                if( !!coord ) {
                    found_opt = run_for_point_in<int, point>( act_mod_map, *coord,
                    [&cursel]( const std::pair<int, inclusive_rectangle<point>> &p ) {
                        cursel[1] = p.first;
                    } );
                }
                if( found_opt ) {
                    active_header = 1;
                    if( action == "SELECT" ) {
                        action = "CONFIRM";
                    }
                }
            }
        }

        if( navigate_ui_list( action, cursel[active_header], scroll_rate, recmax, true ) ) {
            recalc_start = true;
        } else if( action == "LEFT" || action == "RIGHT" ) {
            active_header = inc_clamp_wrap( active_header, action == "RIGHT", headers.size() );
            recalc_start = true;
        } else if( action == "CONFIRM" ) {
            const std::vector<mod_id> &current_tab_mods = all_tabs[iCurrentTab].mods;
            if( active_header == 0 && !current_tab_mods.empty() ) {
                // try-add
                mman_ui->try_add( current_tab_mods[cursel[0]], active_mod_order );
            } else if( active_header == 1 && !active_mod_order.empty() ) {
                // try-rem
                mman_ui->try_rem( cursel[1], active_mod_order );
                if( active_mod_order.empty() ) {
                    // switch back to other list, we can't change
                    // anything in the empty active mods list.
                    active_header = 0;
                }
            }
        } else if( action == "MOVE_MOD_DOWN" ) {
            if( active_header == 1 && active_mod_order.size() > 1 ) {
                mman_ui->try_shift( '+', cursel[1], active_mod_order );
            }
            recalc_start = true;
        } else if( action == "MOVE_MOD_UP" ) {
            if( active_header == 1 && active_mod_order.size() > 1 ) {
                mman_ui->try_shift( '-', cursel[1], active_mod_order );
            }
            recalc_start = true;
        } else if( action == "NEXT_CATEGORY_TAB" ) {
            if( active_header == 0 ) {
                if( ++iCurrentTab >= get_mod_list_tabs().size() ) {
                    iCurrentTab = 0;
                }

                startsel[0] = 0;
                cursel[0] = 0;
                recalc_start = true;
            }

        } else if( action == "PREV_CATEGORY_TAB" ) {
            if( active_header == 0 ) {
                if( --iCurrentTab > get_mod_list_tabs().size() ) {
                    iCurrentTab = get_mod_list_tabs().size() - 1;
                }

                startsel[0] = 0;
                cursel[0] = 0;
                recalc_start = true;
            }
        } else if( action == "NEXT_TAB" ) {
            tab_output = 1;
        } else if( action == "PREV_TAB" ) {
            tab_output = -1;
        } else if( action == "SAVE_DEFAULT_MODS" ) {
            if( mman->set_default_mods( active_mod_order ) ) {
                popup( _( "Saved list of active mods as default" ) );
                draw_modselection_borders( win, ctxt );
            }
        } else if( action == "VIEW_MOD_DESCRIPTION" ) {
            if( const MOD_INFORMATION *selmod = get_selected_mod() ) {
                popup( "%s", mman_ui->get_information( selmod ) );
            }
        } else if( action == "QUIT" ) {
            tab_output = -999;
        } else if( action == "FILTER" ) {
            set_filter();
            recalc_start = true;
        }
        // RESOLVE INPUTS
        if( active_mod_order.empty() ) {
            cursel[1] = 0;
        }

        if( active_header == 1 ) {
            if( active_mod_order.empty() ) {
                cursel[1] = 0;
            } else {
                // If it goes below 0, it'll loop back to max (or at least, greater than AMO size*10.
                if( cursel[1] > active_mod_order.size() * 10 ) {
                    cursel[1] = 0;
                }
                // If it goes above AMO.size(), cap to size.
                else if( cursel[1] >= active_mod_order.size() ) {
                    cursel[1] = active_mod_order.size() - 1;
                }
            }
        }
        // end RESOLVE INPUTS
    }
    return tab_output;
}

static std::string get_opt_slider( int width, int current, int max, bool no_color,
                                   bool no_selector = false )
{
    int new_cur = clamp<int>( std::round( ( width * current ) / static_cast<float>( max ) ),
                              0, width - 1 );
    if( no_selector ) {
        new_cur = -2;
    }

    std::string ret;
    for( int i = 0; i < width; i++ ) {
        char ch = '-';
        if( i == new_cur - 1 ) {
            ch = '<';
        } else if( i == new_cur + 1 ) {
            ch = '>';
        } else if( i == new_cur ) {
            ch = '|';
        }
        if( !no_color && ch != '-' ) {
            ret.append( colorize( std::string( 1, ch ), c_yellow ) );
        } else {
            ret.append( 1, ch );
        }
    }

    return ret;
}

int worldfactory::show_worldgen_basic( WORLD *world )
{
    catacurses::window w_confirmation;

    ui_adaptor ui;

    const point namebar_pos( 3 + utf8_width( _( "World name:" ) ), 1 );

    input_context ctxt( "WORLDGEN_CONFIRM_DIALOG" );
    // dialog actions
    ctxt.register_action( "WORLDGEN_CONFIRM.QUIT" );
    ctxt.register_action( "PICK_RANDOM_WORLDNAME" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "PICK_MODS" );
    ctxt.register_action( "ADVANCED_SETTINGS" );
    ctxt.register_action( "FINALIZE" );
    ctxt.register_action( "RANDOMIZE" );
    ctxt.register_action( "RESET" );
    ctxt.register_leftright();
    ctxt.register_navigate_ui_list();
    // mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );

    int win_height = 0;
    int content_height = 0; // buttons & sliders
    bool recalc_startpos = false;
    const auto init_windows = [&]( ui_adaptor & ui ) {
        recalc_startpos = true;
        const int iMinScreenWidth = std::max( FULL_SCREEN_WIDTH, TERMX / 2 );
        const int iOffsetX = TERMX > FULL_SCREEN_WIDTH ? ( TERMX - iMinScreenWidth ) / 2 : 0;

        w_confirmation = catacurses::newwin( TERMY, iMinScreenWidth, point( iOffsetX, 0 ) );

        win_height = getmaxy( w_confirmation );
        content_height = win_height - namebar_pos.y - 10;

        ui.position_from_window( w_confirmation );
    };
    init_windows( ui );
    ui.on_screen_resize( init_windows );

    bool noname = false;
    bool custom_opts = false;

    std::map<int, inclusive_rectangle<point>> btn_map;
    std::map<int, inclusive_rectangle<point>> slider_inc_map;
    std::vector<option_slider_id> wg_sliders; // option sliders
    std::vector<int> wg_slevels; // current slider levels
    std::string worldname = world->world_name;
    int sel_opt = 0;
    int top_opt = 0;

    for( const option_slider &osl : option_slider::get_all() ) {
        if( osl.context() == "WORLDGEN" ) {
            wg_sliders.emplace_back( osl.id );
            wg_slevels.emplace_back( osl.default_level() );
        }
    }
    const std::vector<int> wg_slvl_default = wg_slevels; // save default slider levels

    ui.on_redraw( [&]( const ui_adaptor & ) {
        slider_inc_map.clear();
        btn_map.clear();
        const int win_width = getmaxx( w_confirmation ) - 2;

        int start = top_opt == 0 ? 0 : ( 2 + ( top_opt - 1 ) * 3 );
        int sel_y = sel_opt == 0 ? 0 :
                    ( sel_opt <= static_cast<int>( wg_sliders.size() ) ?
                      ( 2 + ( sel_opt - 1 ) * 3 ) : ( 2 + wg_sliders.size() * 3 ) );
        if( recalc_startpos ) {
            calcStartPos( start, sel_y, content_height, wg_sliders.size() * 3 + 4 );
        }
        top_opt = start < 2 ? 0 : start / 3 + 1;

        werase( w_confirmation );
        //~ Title text for the world creation menu.  The < and > characters decorate the border.
        draw_border( w_confirmation, BORDER_COLOR, _( "< Create World >" ), c_white );

        if( top_opt == 0 ) {
            // World name
            mvwprintz( w_confirmation, point( 2, namebar_pos.y ), c_white, _( "World name:" ) );
            size_t name_txt_width = 0;
            if( noname ) {
                const std::string name_txt = _( "________NO NAME ENTERED!________" );
                name_txt_width = utf8_width( name_txt );
                mvwprintz( w_confirmation, namebar_pos,
                           sel_opt == 0 ? hilite( c_light_gray ) : c_light_gray, name_txt );
                wnoutrefresh( w_confirmation );
            } else {
                mvwprintz( w_confirmation, namebar_pos, sel_opt == 0 ? hilite( c_pink ) : c_pink, worldname );
                name_txt_width = utf8_width( worldname );
            }
            btn_map.emplace( 0, inclusive_rectangle<point>( namebar_pos,
                             namebar_pos + point( name_txt_width, 0 ) ) );
        }

        // Slider options
        int y = namebar_pos.y + ( top_opt == 0 ? 2 : 0 );
        bool all_sliders_drawn = false;
        for( int i = top_opt == 0 ? 0 : top_opt - 1;
             i < static_cast<int>( wg_sliders.size() ) && y < content_height - 2; i++, y++ ) {
            std::string sl_txt = get_opt_slider( win_width / 2 - 2, wg_slevels[i],
                                                 wg_sliders[i]->count() - 1,
                                                 i == sel_opt - 1, custom_opts );
            trim_and_print( w_confirmation, point( 3, y++ ), win_width,
                            c_white, wg_sliders[i]->name().translated() );
            trim_and_print( w_confirmation, point( 3, y ), win_width,
                            i == sel_opt - 1 ? hilite( c_white ) : c_white, sl_txt );
            if( i == sel_opt - 1 ) {
                mvwputch( w_confirmation, point( 1, y ), hilite( c_yellow ), '<' );
                mvwputch( w_confirmation, point( 2 + win_width / 2, y ), hilite( c_yellow ), '>' );
            }
            slider_inc_map.emplace( i * 2, inclusive_rectangle<point>( point( 1, y ), point( 1, y ) ) );
            slider_inc_map.emplace( i * 2 + 1, inclusive_rectangle<point>( point( 2 + win_width / 2, y ),
                                    point( 2 + win_width / 2, y ) ) );
            mvwprintz( w_confirmation, point( 5 + win_width / 2, y++ ), c_white,
                       custom_opts ? _( "Custom" ) : wg_sliders[i]->level_name( wg_slevels[i] ).translated() );
            btn_map.emplace( 1 + i,
                             inclusive_rectangle<point>( point( 1, y - 1 ), point( 2 + win_width / 2, y - 1 ) ) );
            if( i == static_cast<int>( wg_sliders.size() ) - 1 ) {
                all_sliders_drawn = true;
            }
        }

        auto get_clr = []( const nc_color & base, bool hi ) {
            return hi ? hilite( base ) : base;
        };

        if( all_sliders_drawn && y <= content_height ) {
            int opt_num = wg_sliders.size() + 1;
            nc_color acc_clr;
            nc_color acc_clr2;
            nc_color base_clr;
            std::string btn_txt;
            auto add_button = [&]( const char *action, const char *label, int x ) {
                const bool hi = sel_opt == opt_num;
                acc_clr = get_clr( c_yellow, hi );
                acc_clr2 = get_clr( c_light_green, hi );
                base_clr = get_clr( c_white, hi );
                btn_txt = string_format( "%s%s%s %s %s", colorize( "[", acc_clr ),
                                         colorize( ctxt.get_desc( action,     1U ), acc_clr2 ),
                                         colorize( "][", acc_clr ), label, colorize( "]", acc_clr ) );
                const point pos( x - utf8_width( btn_txt, true ) / 2, y );
                print_colored_text( w_confirmation, pos, base_clr, base_clr, btn_txt );
                btn_map.emplace( opt_num ++,
                                 inclusive_rectangle<point>( pos, pos + point( utf8_width( btn_txt, true ), 0 ) ) );
            };
            add_button( "FINALIZE", _( "Finish" ), win_width / 4 );
            add_button( "RESET", _( "Reset" ), win_width / 2 );
            add_button( "RANDOMIZE", _( "Randomize" ), win_width * 3 / 4 );
            y++;
        }

        // Content scrollbar
        scrollbar()
        .border_color( BORDER_COLOR )
        .offset_x( 0 )
        .offset_y( 1 )
        .content_size( wg_sliders.size() * 3 + 3 )
        .viewport_pos( top_opt * 3 )
        .viewport_size( content_height )
        .apply( w_confirmation );

        // Bottom box
        wattron( w_confirmation, BORDER_COLOR );
        mvwaddch( w_confirmation, point( 0,             win_height - 10 ), LINE_XXXO );
        mvwhline( w_confirmation, point( 1,             win_height - 10 ), LINE_OXOX, win_width - 2 );
        mvwaddch( w_confirmation, point( win_width - 1, win_height - 10 ), LINE_XOXX );
        wattroff( w_confirmation, BORDER_COLOR );

        // Hint text
        std::string hint_txt =
            string_format( _( "Press [<color_yellow>%s</color>] to pick a random name for your world.\n"
                              "Navigate options with [<color_yellow>directional keys</color>] "
                              "and confirm with [<color_yellow>%s</color>].\n"
                              "Press [<color_yellow>%s</color>] to see additional control information." ),
                           ctxt.get_desc( "PICK_RANDOM_WORLDNAME", 1U ), ctxt.get_desc( "CONFIRM", 1U ),
                           ctxt.get_desc( "HELP_KEYBINDINGS", 1U ) );
        if( !custom_opts && sel_opt > 0 && sel_opt <= static_cast<int>( wg_sliders.size() ) ) {
            hint_txt = wg_sliders[sel_opt - 1]->level_desc( wg_slevels[sel_opt - 1] ).translated();
        }
        y += fold_and_print( w_confirmation, point( 2, win_height - 9 ),
                             win_width - 1, c_light_gray, hint_txt ) + 1;

        // Advanced settings legend
        nc_color dummy = c_light_gray;
        std::string sctxt = string_format( _( "[<color_yellow>%s</color>] - Advanced options" ),
                                           ctxt.get_desc( "ADVANCED_SETTINGS", 1U ) );
        mvwprintz( w_confirmation, point( 2, win_height - 4 ), c_light_gray, _( "Advanced settings:" ) );
        print_colored_text( w_confirmation, point( 2, win_height - 3 ), dummy, c_light_gray, sctxt );
        btn_map.emplace( static_cast<int>( wg_sliders.size() + 4 ),
                         inclusive_rectangle<point>(
                             point( 2, win_height - 3 ),
                             point( 2 + utf8_width( sctxt, true ), win_height - 3 ) ) );
        sctxt = string_format( _( "[<color_yellow>%s</color>] - Open mod manager" ),
                               ctxt.get_desc( "PICK_MODS", 1U ) );
        print_colored_text( w_confirmation, point( 2, win_height - 2 ), dummy, c_light_gray, sctxt );
        btn_map.emplace( static_cast<int>( wg_sliders.size() + 5 ),
                         inclusive_rectangle<point>(
                             point( 2, win_height - 2 ),
                             point( 2 + utf8_width( sctxt, true ), win_height - 2 ) ) );
        wnoutrefresh( w_confirmation );
    } );

    do {
        ui_manager::redraw();

        recalc_startpos = false;
        std::string action = ctxt.handle_input();
        // Handle mouse input
        if( action == "MOUSE_MOVE" || action == "SELECT" ) {
            std::optional<point> coord = ctxt.get_coordinates_text( w_confirmation );
            if( !!coord ) {
                int orig_opt = sel_opt;
                bool found = run_for_point_in<int, point>( btn_map, *coord,
                [&sel_opt]( const std::pair<int, inclusive_rectangle<point>> &p ) {
                    sel_opt = p.first;
                } ) > 0;
                if( found && action == "SELECT" ) {
                    if( sel_opt == static_cast<int>( wg_sliders.size() + 4 ) ) {
                        action = "ADVANCED_SETTINGS";
                    } else if( sel_opt == static_cast<int>( wg_sliders.size() + 5 ) ) {
                        action = "PICK_MODS";
                    } else {
                        action = "CONFIRM";
                        run_for_point_in<int, point>( slider_inc_map, *coord,
                        [&action]( const std::pair<int, inclusive_rectangle<point>> &p ) {
                            action = p.first % 2 == 0 ? "LEFT" : "RIGHT";
                        } );
                    }
                }
                if( sel_opt > static_cast<int>( wg_sliders.size() + 3 ) ) {
                    sel_opt = orig_opt;
                }
            }
        }

        // Button shortcuts
        if( action == "FINALIZE" ) {
            action = "CONFIRM";
            sel_opt = wg_sliders.size() + 1;
            ui_manager::redraw();
        } else if( action == "RESET" ) {
            action = "CONFIRM";
            sel_opt = wg_sliders.size() + 2;
            ui_manager::redraw();
        } else if( action == "RANDOMIZE" ) {
            action = "CONFIRM";
            sel_opt = wg_sliders.size() + 3;
            // no confirmation prompt, no need to redraw the ui
        }

        // Handle other inputs
        if( action == "CONFIRM" ) {
            if( sel_opt == 0 ) {
                // rename
                std::optional<std::string> ret = prompt_world_name( _( "World name:" ), worldname );
                if( !ret.value_or( "" ).empty() ) {
                    world->world_name = worldname = ret.value();
                }
            } else if( sel_opt == static_cast<int>( wg_sliders.size() + 1 ) ) {
                // finish
                if( worldname.empty() ) {
                    noname = true;
                    ui.invalidate_ui();
                    if( !query_yn( _( "Are you SURE you're finished?  World name will be randomly generated." ) ) ) {
                        noname = false;
                        continue;
                    } else {
                        noname = false;
                        world->world_name = pick_random_name();
                        if( !valid_worldname( world->world_name ) ) {
                            continue;
                        }
                        return 1;
                    }
                } else if( valid_worldname( worldname ) && query_yn( _( "Are you SURE you're finished?" ) ) ) {
                    world->world_name = worldname;
                    return 1;
                }
            } else if( sel_opt == static_cast<int>( wg_sliders.size() + 2 ) &&
                       query_yn( _( "Are you sure you want to reset this world?" ) ) ) {
                // reset
                world->WORLD_OPTIONS = get_options().get_world_defaults();
                world->world_saves.clear();
                world->active_mod_order = world_generator->get_mod_manager().get_default_mods();
                wg_slevels = wg_slvl_default;
                custom_opts = false;
            } else if( sel_opt == static_cast<int>( wg_sliders.size() + 3 ) ) {
                // randomize
                for( int i = 0; i < static_cast<int>( wg_sliders.size() ); i++ ) {
                    wg_slevels[i] = wg_sliders[i]->random_level();
                }
            }
        } else if( navigate_ui_list( action, sel_opt, 1, wg_sliders.size() + 2, true ) ) {
            recalc_startpos = true;
        } else if( action == "LEFT" || action == "RIGHT" ) {
            if( sel_opt > 0 && sel_opt <= static_cast<int>( wg_sliders.size() ) ) {
                if( custom_opts && query_yn( _( "Currently using customized advanced options.  "
                                                "Reset world options to defaults?" ) ) ) {
                    world->WORLD_OPTIONS = get_options().get_world_defaults();
                    wg_slevels = wg_slvl_default;
                    custom_opts = false;
                    continue;
                } else if( custom_opts ) {
                    continue;
                }
                int lvl = wg_slevels[sel_opt - 1] + ( action == "LEFT" ? -1 : 1 );
                wg_slevels[sel_opt - 1] = clamp<int>( lvl, 0, wg_sliders[sel_opt - 1]->count() - 1 );
                wg_sliders[sel_opt - 1]->apply_opts( wg_slevels[sel_opt - 1], world->WORLD_OPTIONS );
            } else if( sel_opt > static_cast<int>( wg_sliders.size() ) ) {
                if( action == "LEFT" && sel_opt > static_cast<int>( wg_sliders.size() + 1 ) ) {
                    sel_opt--;
                } else if( action == "RIGHT" && sel_opt < static_cast<int>( wg_sliders.size() + 3 ) ) {
                    sel_opt++;
                }
            }
        } else if( action == "PICK_MODS" ) {
            show_worldgen_tab_modselection( w_confirmation, world, false );
        } else if( action == "ADVANCED_SETTINGS" ) {
            options_manager::options_container WOPTIONS_OLD = world->WORLD_OPTIONS;
            show_worldgen_tab_options( w_confirmation, world, false );
            for( auto &iter : WOPTIONS_OLD ) {
                if( iter.second != world->WORLD_OPTIONS[iter.first] ) {
                    custom_opts = true;
                    break;
                }
            }
        } else if( action == "PICK_RANDOM_WORLDNAME" ) {
            world->world_name = worldname = pick_random_name();
        } else if( action == "WORLDGEN_CONFIRM.QUIT" &&
                   query_yn( _( "Do you want to abort World Generation?" ) ) ) {
            world->world_name = worldname;
            return -999;
        }
    } while( true );

    return 0;
}

void worldfactory::draw_modselection_borders( const catacurses::window &win,
        const input_context &ctxtp )
{

    const int iMinScreenWidth = std::max( FULL_SCREEN_WIDTH, TERMX / 2 );

    // make appropriate lines: X & Y coordinate of starting point, length, horizontal/vertical type
    std::array<int, 5> xs = {{1, 1, iMinScreenWidth / 2 + 2, iMinScreenWidth / 2 - 4, iMinScreenWidth / 2 + 2}};
    std::array<int, 5> ys = {{TERMY - 11, 4, 4, 3, 3}};
    std::array<int, 5> ls = {{iMinScreenWidth - 2, iMinScreenWidth / 2 - 4, iMinScreenWidth / 2 - 2, TERMY - 14, 1}};
    std::array<bool, 5> hv = {{true, true, true, false, false}}; // horizontal line = true, vertical line = false

    wattron( win, BORDER_COLOR );

    for( int i = 0; i < 5; ++i ) {
        const point p( xs[i], ys[i] );
        if( hv[i] ) {
            mvwhline( win, p, LINE_OXOX, ls[i] ); // -
        } else {
            mvwvline( win, p, LINE_XOXO, ls[i] ); // |
        }
    }

    // Add in connective characters
    mvwaddch( win, point( 0, 4 ), LINE_XXXO ); // |-
    mvwaddch( win, point( 0, TERMY - 11 ), LINE_XXXO ); // |-
    mvwaddch( win, point( iMinScreenWidth / 2 + 2, 4 ), LINE_XXXO ); // |-

    mvwaddch( win, point( iMinScreenWidth - 1, 4 ), LINE_XOXX ); // -|
    mvwaddch( win, point( iMinScreenWidth - 1, TERMY - 11 ), LINE_XOXX ); // -|
    mvwaddch( win, point( iMinScreenWidth / 2 - 4, 4 ), LINE_XOXX ); // -|

    mvwaddch( win, point( iMinScreenWidth / 2 - 4, 2 ), LINE_OXXX ); // -.-
    mvwaddch( win, point( iMinScreenWidth / 2 + 2, 2 ), LINE_OXXX ); // -.-

    mvwaddch( win, point( iMinScreenWidth / 2 - 4, TERMY - 11 ), LINE_XXOX ); // _|_
    mvwaddch( win, point( iMinScreenWidth / 2 + 2, TERMY - 11 ), LINE_XXOX ); // _|_

    wattroff( win, BORDER_COLOR );

    // Add tips & hints
    fold_and_print( win, point( 2, TERMY - 10 ), getmaxx( win ) - 4, c_light_gray,
                    _( "[<color_yellow>%s</color>] = save <color_cyan>Mod Load Order</color> as default <color_red>|</color> "
                       "[<color_yellow>%s</color>/<color_yellow>%s</color>] = switch Main-Tab <color_red>|</color> "
                       "[<color_yellow>%s</color>/<color_yellow>%s</color>] = switch "
                       "<color_cyan>Mod List</color> and <color_cyan>Mod Load Order</color> <color_red>|</color> "
                       "[<color_yellow>%s</color>/<color_yellow>%s</color>] = switch <color_cyan>Mod List</color> Tab <color_red>|</color> "
                       "[<color_yellow>%s</color>] = keybindings" ),
                    ctxtp.get_desc( "SAVE_DEFAULT_MODS" ),
                    ctxtp.get_desc( "PREV_TAB" ),
                    ctxtp.get_desc( "NEXT_TAB" ),
                    ctxtp.get_desc( "LEFT" ),
                    ctxtp.get_desc( "RIGHT" ),
                    ctxtp.get_desc( "PREV_CATEGORY_TAB" ),
                    ctxtp.get_desc( "NEXT_CATEGORY_TAB" ),
                    ctxtp.get_desc( "HELP_KEYBINDINGS" )
                  );
    wnoutrefresh( win );
}

std::map<size_t, inclusive_rectangle<point>> worldfactory::draw_worldgen_tabs(
            const catacurses::window &w, size_t current )
{
    werase( w );

    static const std::vector<std::string> tab_strings {
        translate_marker( "World Mods" ),
        translate_marker( "World Options" )
    };

    std::vector<std::string> tab_strings_translated( tab_strings );
    std::for_each( tab_strings_translated.begin(),
                   tab_strings_translated.end(), []( std::string & str )->void { str = _( str ); } );

    std::map<size_t, inclusive_rectangle<point>> tab_map =
                draw_tabs( w, tab_strings_translated, current );
    draw_border_below_tabs( w );
    return tab_map;
}

bool worldfactory::valid_worldname( const std::string &name, bool automated ) const
{
    std::string msg;

    if( name.empty() ) {
        msg = _( "World name cannot be empty!" );
    } else if( name == "save" || name == "TUTORIAL" || name == "DEFENSE" ) {
        msg = string_format( _( "%s is a reserved name!" ), name );
    } else if( has_world( name ) ) {
        msg = string_format( _( "A world named %s already exists!" ), name );
    } else {
        // just check the raw bytes because unicode characters are always acceptable
        bool allowed = true;
        for( const char ch : name ) {
            // Convert to unsigned char because `std::isprint` is undefined for
            // values unrepresentable by unsigned char which is not EOF.
            const unsigned char uc = static_cast<unsigned char>( ch );
            if( !is_char_allowed( uc ) ) {
                if( std::isprint( uc ) ) {
                    msg = string_format( _( "World name contains invalid character: '%c'" ), uc );
                } else {
                    msg = string_format( _( "World name contains invalid character: 0x%x" ), uc );
                }
                allowed = false;
                break;
            }
        }
        if( allowed ) {
            return true;
        }
    }
    if( !automated ) {
        popup( msg, PF_GET_KEY );
    }
    return false;
}

bool WORLD::create_timestamp()
{
#if defined( TIME_UTC ) && !defined( MACOSX ) && !defined(__ANDROID__)
    std::timespec t;
    if( std::timespec_get( &t, TIME_UTC ) != TIME_UTC ) {
        return false;
    }
#else
    // MinGW-w64 with pthread, MacOS, Android, etc
    timespec t;
    if( clock_gettime( CLOCK_REALTIME, &t ) != 0 ) {
        return false;
    }
#endif

    std::array<char, sizeof( "yyyymmddHHMMSS" )> ts;
    // Using UTC time instead of local time with time zone offset, because %z
    // returns the localized time zone name instead of the time zone offset on
    // MinGW-w64, which does not conform to the standard.
    const std::size_t ts_len = strftime( ts.data(), ts.size(), "%Y%m%d%H%M%S",
                                         std::gmtime( &t.tv_sec ) );
    if( !ts_len ) {
        return false;
    }

    std::ostringstream str;
    str.imbue( std::locale::classic() );
    str << std::string_view( ts.data(), ts_len );
    str << std::setw( 9 ) << std::setfill( '0' ) << t.tv_nsec;
    timestamp = str.str();
    return true;
}

bool WORLD::save_timestamp() const
{
    if( timestamp.empty() ) {
        return true;
    }

    const cata_path path = folder_path() / PATH_INFO::world_timestamp();
    return write_to_file( path, [this]( std::ostream & file ) {
        JsonOut jsout( file );
        jsout.write( timestamp );
    }, _( "world timestamp" ) );
}

bool WORLD::load_timestamp()
{
    const cata_path path = folder_path() / PATH_INFO::world_timestamp();
    return read_from_file_optional_json( path, [this]( const JsonValue & jv ) {
        const std::string ts = jv.get_string();
        // Sanitize the string since it is used in paths
        for( const char ch : ts ) {
            if( ch < '0' || ch > '9' ) {
                jv.throw_error( "Invalid character encountered in world timestamp." );
            }
        }
        timestamp = ts;
    } );
}

void WORLD::load_options( const JsonArray &options_json )
{
    options_manager &opts = get_options();

    for( JsonObject jo : options_json ) {
        jo.allow_omitted_members();
        const std::string name = opts.migrateOptionName( jo.get_string( "name" ) );
        const std::string value = opts.migrateOptionValue( jo.get_string( "name" ),
                                  jo.get_string( "value" ) );

        if( opts.has_option( name ) && opts.get_option( name ).getPage() == "world_default" ) {
            WORLD_OPTIONS[ name ].setValue( value );
        }
    }
    // for legacy saves, try to simulate old city_size based density
    if( WORLD_OPTIONS.count( "CITY_SPACING" ) == 0 ) {
        WORLD_OPTIONS["CITY_SPACING"].setValue( 5 - get_option<int>( "CITY_SIZE" ) / 3 );
    }
}

bool WORLD::load_options()
{
    WORLD_OPTIONS = get_options().get_world_defaults();

    const cata_path path = folder_path() / PATH_INFO::worldoptions();
    return read_from_file_optional_json( path, [this]( const JsonValue & jsin ) {
        this->load_options( jsin );
    } );
}

void load_world_option( const JsonObject &jo )
{
    JsonArray arr = jo.get_array( "options" );
    if( arr.empty() ) {
        jo.throw_error_at( "options", "no options specified" );
    }
    for( const std::string line : arr ) {
        get_options().get_option( line ).setValue( "true" );
    }
}

//load external option from json
void load_external_option( const JsonObject &jo )
{
    std::string name = jo.get_string( "name" );
    std::string stype = jo.get_string( "stype" );
    bool stub = jo.get_bool( "stub", false );
    // This is a hack to aid in migrating options to external without overriding already-set options.
    // See doc/JSON/OPTIONS.md for more information.
    if( stub ) {
        jo.allow_omitted_members();
        return;
    }
    options_manager &opts = get_options();
    if( !opts.has_option( name ) ) {
        opts.add_external( name, "external_options", stype );
    }
    options_manager::cOpt &opt = opts.get_option( name );
    // TODO: Hook up to cata_variant instead?
    if( stype == "float" ) {
        opt.setValue( static_cast<float>( jo.get_float( "value" ) ) );
    } else if( stype == "int" ) {
        opt.setValue( jo.get_int( "value" ) );
    } else if( stype == "bool" ) {
        if( jo.get_bool( "value" ) ) {
            opt.setValue( "true" );
        } else {
            opt.setValue( "false" );
        }
    } else if( stype == "string" || stype == "string_input" ) {
        opt.setValue( jo.get_string( "value" ) );
    } else {
        jo.throw_error_at( "stype", "Unknown or unsupported stype for external option" );
    }
    options_manager::update_options_cache();
}

bool WORLD::has_compression_enabled() const
{
    cata_path world_folder_path = folder_path();
    return std::filesystem::exists( ( world_folder_path / "maps.dict" ).get_unrelative_path() ) ||
           std::filesystem::exists( ( world_folder_path / "mmr.dict" ).get_unrelative_path() ) ||
           std::filesystem::exists( ( world_folder_path / "overmaps.dict" ).get_unrelative_path() );
}

bool WORLD::set_compression_enabled( bool enabled ) const
{
    // Return immediately if we're already in the desired state.
    if( enabled == has_compression_enabled() ) {
        return true;
    }
    static_popup popup;
    cata_path world_folder_path = folder_path();
    if( enabled ) {
        cata_path dictionary_folder = PATH_INFO::compression_folder_path();
        cata_path maps_dict = dictionary_folder / "maps.dict";
        cata_path mmr_dict = dictionary_folder / "mmr.dict";
        cata_path overmaps_dict = dictionary_folder / "overmaps.dict";

        std::vector<cata_path> folders_to_clean;
        std::vector<cata_path> files_to_clean;

        {
            std::vector<cata_path> maps_folders = get_directories( world_folder_path / "maps" );
            std::filesystem::path maps_dict_path = maps_dict.get_unrelative_path();
            size_t done = 0;
            for( const cata_path &map_folder : maps_folders ) {
                popup.message( _( "Compressing maps [%d/%d]" ), done++, maps_folders.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();
                if( !zzip::create_from_folder( ( map_folder + ".zzip" ).get_unrelative_path(),
                                               map_folder.get_unrelative_path(), maps_dict_path ) ) {
                    return false;
                }
            }
            folders_to_clean = std::move( maps_folders );
        }
        {
            std::vector<cata_path> overmaps = get_files_from_path( "o.", world_folder_path );
            files_to_clean.reserve( files_to_clean.size() + overmaps.size() );
            size_t done = 0;
            std::error_code ec;
            assure_dir_exist( world_folder_path / "overmaps" );
            std::filesystem::path world_folder_unrelative_path = world_folder_path.get_unrelative_path();
            for( const cata_path &overmap : overmaps ) {
                // Some random other files might have `o.` in the name. We only care about the actual
                // overmap files whose names start with `o.`.
                std::filesystem::path overmap_file_path = overmap.get_unrelative_path();
                std::filesystem::path overmap_file_name = overmap_file_path.filename();
                if( overmap_file_name.generic_u8string().find( "o." ) != 0 ) {
                    continue;
                }
                popup.message( _( "Compressing overmaps [%d/%d]" ), done++, overmaps.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();

                // Each overmap gets put into its own zzip indexed by its own file name.
                std::shared_ptr overmap_zzip = zzip::create_from_folder_with_files( (
                                                   world_folder_path / "overmaps" / overmap_file_name + ".zzip" ).get_unrelative_path(),
                                               world_folder_unrelative_path, { overmap_file_path }, 0,
                                               overmaps_dict.get_unrelative_path() );
                if( !overmap_zzip ) {
                    return false;
                }
                files_to_clean.push_back( overmap );
            }
        }
        {
            std::vector<cata_path> character_map_memories;
            size_t done = 0;
            // Each of these is a folder for per-character map memory.
            // We compress each into a zzip_stack, with the same folder name for simplicity.
            // Each map memory region file inside the folders is compressed separately.
            character_map_memories = get_files_from_path( ".mm1", folder_path(), false, true );
            for( const cata_path &character_map_memory_folder : character_map_memories ) {
                popup.message( _( "Compressing map memory [%d/%d]" ), done++, character_map_memories.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();

                std::vector<cata_path> character_map_memory_files = get_files_from_path( ".mmr",
                        character_map_memory_folder, false, true );
                std::filesystem::path mmr_path = character_map_memory_folder.get_unrelative_path();
                for( const cata_path &map_memory : character_map_memory_files ) {
                    std::filesystem::path map_memory_filename = map_memory.get_unrelative_path().filename();
                    std::shared_ptr<zzip_stack> map_memory_zzip = zzip_stack::create_from_folder_with_files(
                                character_map_memory_folder.get_unrelative_path(),
                                mmr_path, { mmr_path / map_memory_filename }, 0,
                                mmr_dict.get_unrelative_path() );
                    if( !map_memory_zzip ) {
                        return false;
                    }
                }

                files_to_clean.insert( files_to_clean.end(), character_map_memory_files.begin(),
                                       character_map_memory_files.end() );
            }
        }
        {
            std::vector<cata_path> saves;
            size_t done = 0;
            std::error_code ec;
            saves = get_files_from_path( ".sav", world_folder_path );
            for( const cata_path &save : saves ) {
                popup.message( _( "Compressing main save files [%d/%d]" ), done++, saves.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();

                // Each save gets put into its own zzip indexed by its own file name.
                std::filesystem::path save_file_path = save.get_unrelative_path();
                std::filesystem::path save_file_name = save_file_path.filename();

                // But first, we have to do some surgery.
                // Current the first line of a save file is a hokey line like
                // # version <number>
                // This is outdated from a time when the file had to be loaded linearly
                // in text. We now parse to a binary format and can random access anything
                // in constant time. The new format encodes the version as a regular json
                // member. The compressed save load path requires that. So first we test if
                // the first line is this format and we insert it manually textually if it is.
                std::string savefile_contents = read_entire_file( save_file_path );
                if( savefile_contents.empty() ) {
                    // Eh just skip it.
                    continue;
                }
                if( savefile_contents[0] == '#' ) {
                    // Parse the version header.
                    std::string temp_savefile_contents = std::move( savefile_contents );
                    savefile_contents.clear();
                    size_t newline = temp_savefile_contents.find( '\n' );
                    size_t char_after_open_brace = temp_savefile_contents.find_first_not_of( '{', newline + 1 );
                    std::string_view header{ temp_savefile_contents.data(), newline };
                    std::string_view savefile_json{ temp_savefile_contents.data() + char_after_open_brace, temp_savefile_contents.size() - char_after_open_brace };
                    int temp_savefile_version = std::strtol( header.data() + header.find_last_of( ' ' ), nullptr, 10 );
                    savefile_contents.reserve( savefile_json.size() + 32 ); // 30 for text and 2 for digits.
                    savefile_contents.append( "{\"savegame_loading_version\":" );
                    savefile_contents.append( std::to_string( temp_savefile_version ) );
                    savefile_contents.append( ",\n" );
                    savefile_contents.append( savefile_json );
                };

                std::shared_ptr save_zzip = zzip::load( ( world_folder_path / save_file_name +
                                                        ".zzip" ).get_unrelative_path() );
                if( !save_zzip ) {
                    return false;
                }
                save_zzip->add_file( save_file_name, savefile_contents );
                files_to_clean.push_back( save );
            }
        }
        copy_file( maps_dict, folder_path() / "maps.dict" );
        copy_file( overmaps_dict, folder_path() / "overmaps.dict" );
        copy_file( mmr_dict, folder_path() / "mmr.dict" );
        size_t done = 0;
        size_t to_do = folders_to_clean.size() + files_to_clean.size();
        for( const cata_path &folder : folders_to_clean ) {
            popup.message( _( "Cleaning up [%d/%d]" ), done++, to_do );
            ui_manager::redraw();
            refresh_display();
            inp_mngr.pump_events();
            std::error_code ec;
            std::filesystem::remove_all( folder.get_unrelative_path(), ec );
        }
        for( const cata_path &file : files_to_clean ) {
            popup.message( _( "Cleaning up [%d/%d]" ), done++, to_do );
            ui_manager::redraw();
            refresh_display();
            inp_mngr.pump_events();
            std::error_code ec;
            std::filesystem::remove( file.get_unrelative_path(), ec );
        }
    } else {
        cata_path maps_dict = world_folder_path / "maps.dict";
        cata_path overmaps_dict = world_folder_path / "overmaps.dict";
        cata_path mmr_dict = world_folder_path / "mmr.dict";
        std::vector<cata_path> zzips_to_clean;

        std::vector<cata_path> maps_zzips = get_files_from_path( "zzip", folder_path() / "maps", false,
                                            true );
        std::vector<cata_path> overmap_zzips = get_files_from_path( "zzip", folder_path() / "overmaps",
                                               false, true );
        std::vector<cata_path> character_map_memory_folders = get_files_from_path( ".mm1",
                world_folder_path, false, true );
        std::vector<cata_path> save_zzips = get_files_from_path( ".sav.zzip", world_folder_path,
                                            false, true );

        zzips_to_clean.reserve( maps_zzips.size() + overmap_zzips.size() +
                                character_map_memory_folders.size() * 3 );

        size_t done = 0;
        {
            std::filesystem::path maps_dict_path = maps_dict.get_unrelative_path();
            for( const cata_path &map_zzip : maps_zzips ) {
                popup.message( _( "Decompressing maps [%d/%d]" ), done++, maps_zzips.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();
                std::filesystem::path zzip_path = map_zzip.get_unrelative_path();
                std::filesystem::path dest_folder_name = zzip_path.parent_path() / zzip_path.stem();
                if( !zzip::extract_to_folder( zzip_path, dest_folder_name, maps_dict_path ) ) {
                    return false;
                }
            }
            zzips_to_clean.insert( zzips_to_clean.end(), maps_zzips.begin(), maps_zzips.end() );
        }
        {
            size_t done = 0;
            std::filesystem::path overmaps_dict_path = overmaps_dict.get_unrelative_path();
            zzips_to_clean.reserve( zzips_to_clean.size() + overmap_zzips.size() );
            std::filesystem::path dest_folder_name = folder_path().get_unrelative_path();
            for( cata_path &overmap_zzip : overmap_zzips ) {

                popup.message( _( "Decompressing overmaps [%d/%d]" ), done++, overmap_zzips.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();

                std::filesystem::path zzip_path = overmap_zzip.get_unrelative_path();
                if( !zzip::extract_to_folder( zzip_path, dest_folder_name, overmaps_dict_path ) ) {
                    return false;
                }
                zzips_to_clean.push_back( std::move( overmap_zzip ) );
            }
            zzips_to_clean.push_back( world_folder_path / "overmaps" );
        }
        {
            size_t done = 0;
            std::filesystem::path mmr_dict_path = mmr_dict.get_unrelative_path();
            std::vector<cata_path> character_map_memory_zzips;
            for( const cata_path &character_map_memory_zzip : character_map_memory_folders ) {
                popup.message( _( "Decompressing map memory [%d/%d]" ), done++,
                               character_map_memory_folders.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();

                std::filesystem::path zzip_path = character_map_memory_zzip.get_unrelative_path();
                // We reuse the same folder for the map memory files.
                const std::filesystem::path &dest_folder_name = zzip_path;
                if( !zzip_stack::extract_to_folder( zzip_path, dest_folder_name, mmr_dict_path ) ) {
                    return false;
                }

                character_map_memory_zzips.emplace_back( character_map_memory_zzip /
                        dest_folder_name.filename().concat( ".cold.zzip" ) ); // NOLINT(cata-u8-path)
                character_map_memory_zzips.emplace_back( character_map_memory_zzip /
                        dest_folder_name.filename().concat( ".warm.zzip" ) ); // NOLINT(cata-u8-path)
                character_map_memory_zzips.emplace_back( character_map_memory_zzip /
                        dest_folder_name.filename().concat( ".hot.zzip" ) ); // NOLINT(cata-u8-path)
            }
            zzips_to_clean.insert( zzips_to_clean.end(), character_map_memory_zzips.begin(),
                                   character_map_memory_zzips.end() );
        }
        {
            size_t done = 0;
            std::filesystem::path dest_folder_name = world_folder_path.get_unrelative_path();
            for( cata_path &save_zzip : save_zzips ) {

                popup.message( _( "Decompressing main save files [%d/%d]" ), done++, overmap_zzips.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();

                std::filesystem::path zzip_path = save_zzip.get_unrelative_path();
                if( !zzip::extract_to_folder( zzip_path, dest_folder_name ) ) {
                    return false;
                }
                zzips_to_clean.push_back( std::move( save_zzip ) );
            }
        }
        remove_file( maps_dict );
        remove_file( overmaps_dict );
        remove_file( mmr_dict );
        done = 0;
        for( const cata_path &zzip_to_clean : zzips_to_clean ) {
            popup.message( _( "Cleaning up [%d/%d]" ), done++, zzips_to_clean.size() );
            ui_manager::redraw();
            refresh_display();
            inp_mngr.pump_events();
            std::error_code ec;
            std::filesystem::remove( zzip_to_clean.get_unrelative_path(), ec );
        }
    }
    return true;
}

mod_manager &worldfactory::get_mod_manager()
{
    return *mman;
}

WORLD *worldfactory::get_world( const std::string &name )
{
    const auto iter = all_worlds.find( name );
    if( iter == all_worlds.end() ) {
        debugmsg( "Requested non-existing world %s, prepare for crash", name );
        return nullptr;
    }
    return iter->second.get();
}

std::string worldfactory::get_world_name( const size_t index )
{
    size_t i = 0;
    for( const auto &elem : all_worlds ) {
        if( i == index ) {
            return elem.first;
        }
        i++;
    }
    return "";
}

size_t worldfactory::get_world_index( const std::string &name )
{
    size_t i = 0;
    for( const auto &elem : all_worlds ) {
        if( elem.first == name ) {
            return i;
        }
        i++;
    }
    return 0;
}

// Helper predicate to exclude files from deletion when resetting a world directory.
static bool isForbidden( const cata_path &candidate )
{
    std::filesystem::path candidate_path = candidate.get_unrelative_path();
    std::string filename = candidate_path.filename().generic_u8string();
    return filename == PATH_INFO::worldoptions()
           || filename == "mods.json"
           || candidate_path.extension().generic_u8string() == ".dict";
}

void worldfactory::delete_world( const std::string &worldname, const bool delete_folder )
{
    cata_path worldpath = get_world( worldname )->folder_path();
    std::set<std::filesystem::path> directory_paths;

    if( delete_folder ) {
        std::filesystem::remove_all( worldpath.get_unrelative_path() );
        remove_world( worldname );
        return;
    }

    // Clear out everything except options and mods and compression dictionaries.
    // It would be easier to delete and recreate the world, but some people,
    // like the author of this code, use symlinks to have world contents located
    // 'elsewhere', and doing so would break such use cases.
    auto file_paths = get_files_from_path( "", worldpath, true, true );
    auto end = std::remove_if( file_paths.begin(), file_paths.end(), isForbidden );
    file_paths.erase( end, file_paths.end() );

    for( cata_path &file_path : file_paths ) {
        std::filesystem::path folder_path = file_path.get_unrelative_path().parent_path();
        while( folder_path.filename() != std::filesystem::u8path( worldname ) ) {
            directory_paths.insert( folder_path );
            folder_path = folder_path.parent_path();
        }
    }

    for( cata_path &file : file_paths ) {
        remove_file( file );
    }

    // Trying to remove a non-empty parent directory before a child
    // directory will fail.  Removing directories in reverse order
    // will prevent this situation from arising.
    for( auto it = directory_paths.rbegin(); it != directory_paths.rend(); ++it ) {
        remove_directory( *it );
    }
    get_world( worldname )->world_saves.clear();
}
