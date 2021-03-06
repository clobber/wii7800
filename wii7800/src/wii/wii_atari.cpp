/*
Wii7800 : Port of the ProSystem Emulator for the Wii

Copyright (C) 2010
raz0red (www.twitchasylum.com)

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1.	The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.

2.	Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3.	This notice may not be removed or altered from any source
distribution.
*/

#include "Database.h"
#include "Sound.h"
#include "Timer.h"

#include <gccore.h>

#include "font_ttf.h"

#include "wii_config.h"
#include "wii_gx.h"
#include "wii_hw_buttons.h"
#include "wii_input.h"
#include "wii_sdl.h"
#include "wii_main.h"

#include "wii_atari.h"
#include "wii_atari_input.h"
#include "wii_atari_sdl.h"

// The size of the crosshair
#define CROSSHAIR_SIZE 11
// The offset from the center of the crosshair
#define CROSSHAIR_OFFSET 5
// The amount of time (in seconds) to display the difficulty swtiches when
// they are changed
#define DIFF_DISPLAY_LENGTH 5

// The palette (8-bit)
byte atari_pal8[256] = {0};
// Whether to flash the screen 
BOOL wii_lightgun_flash = TRUE;
// Whether to display a crosshair for the lightgun
BOOL wii_lightgun_crosshair = TRUE;
// Whether wsync is enabled/disabled
u8 wii_cart_wsync = CART_MODE_AUTO;
// Whether cycle stealing is enabled/disabled
u8 wii_cart_cycle_stealing = CART_MODE_AUTO;
// Whether high score cart is enabled
BOOL wii_hs_enabled = TRUE;
// What mode the high score cart is in
BOOL wii_hs_mode = HSMODE_ENABLED_NORMAL;
// Whether to swap buttons
BOOL wii_swap_buttons = FALSE;
// If the difficulty switches are enabled
BOOL wii_diff_switch_enabled = FALSE;
// When to display the difficulty switches
BOOL wii_diff_switch_display = DIFF_SWITCH_DISPLAY_WHEN_CHANGED;
// Auto save snapshot?
BOOL wii_auto_save_snapshot = FALSE;
// Auto load snapshot?
BOOL wii_auto_load_snapshot = TRUE;
// What is the display size?
u8 wii_scale = 1;
// The screen X size
int wii_screen_x = DEFAULT_SCREEN_X;
// The screen Y size
int wii_screen_y = DEFAULT_SCREEN_Y;
// Whether to display debug info (FPS, etc.)
short wii_debug = 0;
// The maximum frame rate
int wii_max_frame_rate = 0;

// The 7800 scanline that the lightgun is currently at
int lightgun_scanline = 0;
// The 7800 cycle that the lightgun is currently at
float lightgun_cycle = 0;
// Whether the lightgun is enabled for the current cartridge
bool lightgun_enabled = false;
// Tracks the first time the lightgun is fired for the current cartridge
bool lightgun_first_fire = true;

// Whether this is a test frame
bool wii_testframe = false;

// Whether the left difficulty switch is on
static bool left_difficulty_down = false;
// Whether the right difficulty switch is on
static bool right_difficulty_down = false;
// The keyboard (controls) data
static unsigned char keyboard_data[19];
// The amount of time to wait before reading the difficulty switches
static int diff_wait_count = 0;
// The amount of time left to display the difficulty switch values
static int diff_display_count = 0;

// The x location of the Wiimote (IR)
int wii_ir_x = -100;
// The y location of the Wiimote (IR)
int wii_ir_y = -100;

// Forward reference
static void wii_atari_display_crosshairs( int x, int y, BOOL erase );

// Initializes the menu
extern void wii_atari_menu_init();

extern "C" void WII_VideoStop();
extern "C" void WII_ChangeSquare(int xscale, int yscale, int xshift, int yshift);
extern "C" void WII_SetRenderCallback( void (*cb)(void) );

// 
// For debug output
//

extern int hs_sram_write_count;
extern unsigned int riot_timer_count;
extern byte riot_drb;
extern byte RANDOM;
extern uint dbg_saved_cycles;
extern uint dbg_wsync_count;
extern uint dbg_maria_cycles;
extern uint dbg_p6502_cycles;
extern bool dbg_wsync;
extern bool dbg_cycle_stealing;

static float wii_fps_counter;
static int wii_dbg_scanlines;

/*
 * Initializes the application
 */
void wii_handle_init()
{  
  logger_Initialize();

  wii_read_config();  

  // Startup the SDL
  if( !wii_sdl_init() ) 
  {
    fprintf( stderr, "FAILED : Unable to init SDL: %s\n", SDL_GetError() );
    exit( EXIT_FAILURE );
  }

  // FreeTypeGX
  InitFreeType( (uint8_t*)font_ttf, (FT_Long)font_ttf_size  );

  sound_Initialize();
  sound_SetMuted( true );

  wii_atari_menu_init();
}

/*
 * Frees resources prior to the application exiting
 */
void wii_handle_free_resources()
{   
  wii_write_config();
  wii_sdl_free_resources();

  // FreeTypeGX
  ClearFontData();

  SDL_Quit();
}

/*
 * Runs the application (main loop)
 */
void wii_handle_run()
{
  WII_VideoStop();

  // Show the menu (starts the menu loop)
  wii_menu_show();
}

/*
 * Initializes the 8-bit palette
 */
static void wii_atari_init_palette8()
{
  const byte *palette;
  if( cartridge_region == REGION_PAL )
  {
    palette = REGION_PALETTE_PAL;
  }
  else
  {
    palette = REGION_PALETTE_NTSC;
  }

  for( uint index = 0; index < 256; index++ ) 
  {
    word r = palette[ (index * 3) + 0 ];
    word g = palette[ (index * 3) + 1 ];
    word b = palette[ (index * 3) + 2 ]; 
    atari_pal8[index] = wii_sdl_rgb( r, g, b );
  }
}

/*
 * Pauses the emulator
 *
 * pause    Whether to pause or resume
 */
void wii_atari_pause( bool pause )
{
  sound_SetMuted( pause );
  prosystem_Pause( pause );

  if( !pause ) 
  {
    timer_Reset();
  }
}

/*
 * Resets the keyboard (control) information
 */
void wii_reset_keyboard_data()
{
  memset( keyboard_data, 0, sizeof(keyboard_data) );

  // Left difficulty switch defaults to off
  keyboard_data[15] = 1;
  left_difficulty_down = false;

  // Right difficulty swtich defaults to on
  keyboard_data[16] = 0;
  right_difficulty_down = true;

  diff_wait_count = prosystem_frequency * 0.75;
  diff_display_count = 0;
}

/*
 * Loads the specified ROM
 *
 * filename     The filename of the ROM
 * loadbios     Whether or not to load the Atari BIOS
 *
 * return   Whether the load was successful
 */
bool wii_atari_load_rom( char *filename, bool loadbios ) 
{
  std::string std_filename( filename );
  if( !cartridge_Load(std_filename) ) return false;

  database_Load( cartridge_digest );

  bios_enabled = false;
  if( loadbios )
  {
    if( bios_Load( 
      ( cartridge_region == REGION_PAL ? 
        WII_ROOT_BOOT_ROM_PAL : WII_ROOT_BOOT_ROM_NTSC  ) ) ) 
    {
      bios_enabled = true;
    }
    else
    {
      bios_enabled = false;
    }
  }

  wii_reset_keyboard_data();
  wii_atari_init_palette8();   
  prosystem_Reset();

  wii_atari_pause( false );

  return true;
}

/*
 * Renders the current frame to the Wii
 */
void wii_atari_put_image_gu_normal()
{
  int atari_height = 
    ( cartridge_region == REGION_PAL ? PAL_ATARI_HEIGHT : NTSC_ATARI_HEIGHT );
  int atari_offsety = 
    ( cartridge_region == REGION_PAL ? PAL_ATARI_BLIT_TOP_Y : NTSC_ATARI_BLIT_TOP_Y ); 
  int offsetx = ( wii_scale == 1 ? ( ( WII_WIDTH - ATARI_WIDTH ) / 2 ) : 0 );
  int offsety = ( wii_scale == 1 ? ( ( WII_HEIGHT - atari_height ) / 2 ) : 0 );

  int src = 0, dst = 0, start = 0, x = 0, y = 0, i = 0;
  byte* backpixels = (byte*)back_surface->pixels;
  byte* blitpixels = (byte*)blit_surface->pixels;
  int startoffset = atari_offsety * ATARI_WIDTH;
  for( y = 0; y < atari_height; y++ )
  {    
    start = startoffset + ( y * ATARI_WIDTH );
    src = 0;
    dst = ( ( ( y * wii_scale ) + offsety ) * WII_WIDTH ) + offsetx;
    for( i = 0; i < wii_scale; i++ )
    {
      for( x = 0; x < ATARI_WIDTH; x++ )
      {                
        for( int j = 0; j < wii_scale; j++ )
        {
          backpixels[dst++] = blitpixels[start + src];
        }
        src++;
      }
    }            
  }
}

/*
 * Displays the Atari difficulty switch settings
 */
static void wii_atari_display_diff_switches()
{
  if( diff_display_count > 0 )
  {
    diff_display_count--;
  }

  if( ( wii_diff_switch_display == DIFF_SWITCH_DISPLAY_ALWAYS ) ||
    ( ( wii_diff_switch_display == DIFF_SWITCH_DISPLAY_WHEN_CHANGED ) &&
    diff_display_count > 0 ) )
  {
    GXColor red = (GXColor){ 0xff, 0x00, 0x00, 0xff };
    GXColor black = (GXColor){ 0x00, 0x00, 0x00, 0xff };

    //wii_sdl_draw_rectangle( 9, 444, 22, 10, 0x0 );
    wii_gx_drawrectangle( -311, -204, 22, 10, black, TRUE );
    if( !keyboard_data[15] ) 
    {
      //wii_sdl_fill_rectangle( 10, 445, 20, 8, color );
      wii_gx_drawrectangle( -310, -205, 20, 8, red, TRUE );
    } 
    else 
    {      
      //wii_sdl_draw_rectangle( 10, 445, 20, 8, color );
      wii_gx_drawrectangle( -310, -205, 20, 8, red, FALSE );
    }

    //wii_sdl_draw_rectangle( 39, 444, 22, 10, 0x0 );
    wii_gx_drawrectangle( -281, -204, 22, 10, black, TRUE );
    if( !keyboard_data[16] ) 
    {
      //wii_sdl_fill_rectangle( 40, 445, 20, 8, color );
      wii_gx_drawrectangle( -280, -205, 20, 8, red, TRUE );
    } 
    else 
    {
      //wii_sdl_draw_rectangle( 40, 445, 20, 8, color );
      wii_gx_drawrectangle( -280, -205, 20, 8, red, FALSE );
    }
  }
}

/* 
 * Refreshes the Wii display
 *
 * sync         Whether vsync is available for the current frame
 * testframes   The number of testframes to run (for loading saves)
 */
static void wii_atari_refresh_screen( bool sync, int testframes )
{        
  if( diff_wait_count > 0 )
  {        
    // Reduces the number of frames remaining to display the difficulty
    // switches.
    diff_wait_count--;
  }

  BOOL drawcrosshair = lightgun_enabled && wii_lightgun_crosshair;
  if( drawcrosshair )
  {
    // Display the crosshairs
    wii_atari_display_crosshairs( wii_ir_x, wii_ir_y, FALSE );
  }

  wii_atari_put_image_gu_normal();    
  
  if( drawcrosshair )
  {
    // Erase the crosshairs
    wii_atari_display_crosshairs( wii_ir_x, wii_ir_y, TRUE );
  }

  if( sync ) 
  {
    wii_sync_video();
  }

  if( testframes < 0 )
  {    
    wii_sdl_flip();
  }
}

/*
 * Displays the crosshairs for the lightgun
 *
 * x      The x location
 * y      The y location
 * erase  Whether we are erasing the crosshairs
 */
static void wii_atari_display_crosshairs( int x, int y, BOOL erase )
{
  if( x < 0 || y < 0 ) return;

  uint color = 
    ( erase ? wii_sdl_rgb( 0, 0, 0 ) : wii_sdl_rgb( 0xff, 0xff, 0xff ) );

  int cx = ( x - CROSSHAIR_OFFSET ) + cartridge_crosshair_x;
  int cy = ( y - CROSSHAIR_OFFSET ) + cartridge_crosshair_y;

  float xratio = (float)ATARI_WIDTH/(float)WII_WIDTH;
  float yratio = (float)NTSC_ATARI_HEIGHT/(float)WII_HEIGHT;

  float x0 = 0;
  float y0 = 0;

  cx = x0 + ( cx * xratio );
  cy = y0 + ( cy * yratio );

  wii_sdl_draw_rectangle( 
    blit_surface, cx, cy + CROSSHAIR_OFFSET, CROSSHAIR_SIZE, 
    1, color, !erase );

  wii_sdl_draw_rectangle( 
    blit_surface, cx + CROSSHAIR_OFFSET, cy, 1, 
    CROSSHAIR_SIZE, color, !erase );  
}

/*
 * Stores the current location of the Wiimote (IR)
 */
static void wii_atari_update_wiimote_ir()
{
  // Necessary as the SDL seems to keep resetting the resolution
  WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

  ir_t ir;
  WPAD_IR(WPAD_CHAN_0, &ir);

  if( ir.valid )
  {
    wii_ir_x = ir.x;
    wii_ir_y = ir.y; 
  }
  else
  {
    wii_ir_x = -100;
    wii_ir_y = -100;
  }
}

// The number of cycles per scanline that the 7800 checks for a hit
#define LG_CYCLES_PER_SCANLINE 318
// The number of cycles indented (after HBLANK) prior to checking for a hit
#define LG_CYCLES_INDENT 52

/*
 * Updates the joystick state
 *
 * joyIndex         The joystick index
 * keyboard_data    The keyboard (controls) state
 */
static void wii_atari_update_joystick( int joyIndex, unsigned char keyboard_data[19] )
{
  // Check the state of the controllers
  u32 down = WPAD_ButtonsDown( joyIndex );
  u32 held = WPAD_ButtonsHeld( joyIndex );
  u32 gcDown = PAD_ButtonsDown( joyIndex );
  u32 gcHeld = PAD_ButtonsHeld( joyIndex );

  // Check to see if the lightgun is enabled (lightgun only works for
  // joystick index 0).
  bool lightgun = ( lightgun_enabled && ( joyIndex == 0 ) );

  if( lightgun )
  {
    // Determine the Y offset of the lightgun location
    int yoffset = ( cartridge_region == REGION_NTSC ? 
      ( NTSC_ATARI_BLIT_TOP_Y ) : ( PAL_ATARI_BLIT_TOP_Y - 28 ) );

    // The number of scanlines for the current cartridge
    int scanlines = ( cartridge_region == REGION_NTSC ? 
                        NTSC_ATARI_HEIGHT : PAL_ATARI_HEIGHT );
    wii_dbg_scanlines = scanlines;

    // We track the first time the lightgun is fired due to the fact that
    // when a catridge is launched (via the Wii7800 menu) the state of the
    // fire button (down) is used to determine whether the console has a
    // joystick or lightgun plugged in.
    if( lightgun_first_fire )
    {
      if( !( held & ( WPAD_BUTTON_B | WPAD_BUTTON_A ) ) )
      {
        // The button is not down, enable lightgun firing.
        lightgun_first_fire = false;
      }            
      keyboard_data[3] = true;
    }
    else
    {
      keyboard_data[3] = !( held & ( WPAD_BUTTON_B | WPAD_BUTTON_A ) );
    }

    //
    // TODO: These values should be cached
    //
    float yratio = ( (float)scanlines / (float)WII_HEIGHT );
    float xratio = ( (float)LG_CYCLES_PER_SCANLINE / (float)WII_WIDTH );
    lightgun_scanline = ( ( (float)wii_ir_y * yratio ) + 
      ( maria_visibleArea.top - maria_displayArea.top + 1 ) + yoffset );
    lightgun_cycle = ( HBLANK_CYCLES + LG_CYCLES_INDENT + 
      ( (float)wii_ir_x * xratio ) );
    if( lightgun_cycle > CYCLES_PER_SCANLINE )
    {
      lightgun_scanline++;
      lightgun_cycle -= CYCLES_PER_SCANLINE; 
    }
  }
  else
  {
    expansion_t exp;
    WPAD_Expansion( joyIndex, &exp );
    bool isClassic = ( exp.type == WPAD_EXP_CLASSIC );

    float expX = wii_exp_analog_val( &exp, TRUE, FALSE );
    float expY = wii_exp_analog_val( &exp, FALSE, FALSE );
    s8 gcX = PAD_StickX( joyIndex );
    s8 gcY = PAD_StickY( joyIndex );

    float expRjsX = 0, expRjsY = 0;
    s8 gcRjsX = 0, gcRjsY = 0;

    // Dual analog support
    if( cartridge_dualanalog && joyIndex == 1  )
    {
      expansion_t exp0;
      WPAD_Expansion( 0, &exp0 );
      if( exp0.type == WPAD_EXP_CLASSIC  )
      {
        expRjsX = wii_exp_analog_val( &exp0, TRUE, TRUE );
        expRjsY = wii_exp_analog_val( &exp0, FALSE, TRUE );
      }

      gcRjsX = PAD_SubStickX( 0 );
      gcRjsY = PAD_SubStickY( 0 );
    }

    int offset = ( joyIndex == 0 ? 0 : 6 );

    // | 00 06     | Joystick 1 2 | Right
    keyboard_data[0 + offset] = 
      ( held & WII_BUTTON_ATARI_RIGHT || gcHeld & GC_BUTTON_ATARI_RIGHT ||
      wii_analog_right( expX, gcX ) || wii_analog_right( expRjsX, gcRjsX ) );
    // | 01 07     | Joystick 1 2 | Left
    keyboard_data[1 + offset] = 
      ( held & ( WII_BUTTON_ATARI_LEFT | ( isClassic ? WII_CLASSIC_ATARI_LEFT : 0 ) ) || 
      gcHeld & GC_BUTTON_ATARI_LEFT || wii_analog_left( expX, gcX ) ||
      wii_analog_left( expRjsX, gcRjsX ) );
    // | 02 08     | Joystick 1 2 | Down
    keyboard_data[2 + offset] = 
      ( held & WII_BUTTON_ATARI_DOWN || gcHeld & GC_BUTTON_ATARI_DOWN || 
      wii_analog_down( expY, gcY ) || wii_analog_down( expRjsY, gcRjsY ) );
    // | 03 09     | Joystick 1 2 | Up
    keyboard_data[3 + offset] = 
      ( held & ( WII_BUTTON_ATARI_UP | ( isClassic ? WII_CLASSIC_ATARI_UP : 0 ) ) || 
      gcHeld & GC_BUTTON_ATARI_UP || wii_analog_up( expY, gcY ) ||
      wii_analog_up( expRjsY, gcRjsY ) );
    // | 04 10     | Joystick 1 2 | Button 1
    keyboard_data[wii_swap_buttons ? 4 + offset : 5 + offset] = 
      ( held & ( WII_BUTTON_ATARI_FIRE | 
      ( isClassic ? WII_CLASSIC_ATARI_FIRE : WII_NUNCHECK_ATARI_FIRE ) ) || 
      gcHeld & GC_BUTTON_ATARI_FIRE );
    // | 05 11     | Joystick 1 2 | Button 2
    keyboard_data[wii_swap_buttons ? 5 + offset : 4 + offset] = 
      ( held & ( WII_BUTTON_ATARI_FIRE_2 | 
      ( isClassic ? WII_CLASSIC_ATARI_FIRE_2 : WII_NUNCHECK_ATARI_FIRE_2 ) ) || 
      gcHeld & GC_BUTTON_ATARI_FIRE_2 );
  }

  if( joyIndex == 0 )
  {
    // | 12       | Console      | Reset
    keyboard_data[12] = ( held & WII_BUTTON_ATARI_RESET || gcHeld & GC_BUTTON_ATARI_RESET );
    // | 13       | Console      | Select
    keyboard_data[13] = ( held & WII_BUTTON_ATARI_SELECT || gcHeld & GC_BUTTON_ATARI_SELECT );
    // | 14       | Console      | Pause               
    keyboard_data[14] = ( held & WII_BUTTON_ATARI_PAUSE || gcHeld & GC_BUTTON_ATARI_PAUSE );

    if( wii_diff_switch_enabled )
    {
      // | 15       | Console      | Left Difficulty
      if( ( diff_wait_count == 0 ) && 
        ( ( gcDown & GC_BUTTON_ATARI_DIFFICULTY_LEFT ) ||
        ( ( !lightgun && ( down & WII_BUTTON_ATARI_DIFFICULTY_LEFT ) ) ||
        ( lightgun && ( down & WII_BUTTON_ATARI_DIFFICULTY_LEFT_LG ) ) ) ) )
      {
        if( !left_difficulty_down )
        {
          keyboard_data[15] = !keyboard_data[15];
          left_difficulty_down = true;   
          diff_display_count = prosystem_frequency * DIFF_DISPLAY_LENGTH;
        }
      }
      else
      {
        left_difficulty_down = false;
      }
      // | 16       | Console      | Right Difficulty        
      if( ( diff_wait_count == 0 ) && 
        ( ( gcDown & GC_BUTTON_ATARI_DIFFICULTY_RIGHT ) ||
        ( ( !lightgun && ( down & WII_BUTTON_ATARI_DIFFICULTY_RIGHT ) ) ||
        ( lightgun && ( down & WII_BUTTON_ATARI_DIFFICULTY_RIGHT_LG ) ) ) ) )                  
      {
        if( !right_difficulty_down )
        {
          keyboard_data[16] = !keyboard_data[16];
          right_difficulty_down = true;    
          diff_display_count = prosystem_frequency * DIFF_DISPLAY_LENGTH;
        }
      }
      else
      {
        right_difficulty_down = false;
      }
    }

    if( ( down & WII_BUTTON_HOME ) || ( gcDown & GC_BUTTON_HOME ) || wii_hw_button )
    {
      wii_atari_pause( true );
    }
  }    
}

/*
 * Updates the Atari keys (controls) state
 *
 * keyboard_data    The keyboard (controls) state
 */
static void wii_atari_update_keys( unsigned char keyboard_data[19] )
{    
  WPAD_ScanPads();
  PAD_ScanPads();        

  if( lightgun_enabled )
  {
    wii_atari_update_wiimote_ir();
  }
  wii_atari_update_joystick( 0, keyboard_data );
  wii_atari_update_joystick( 1, keyboard_data );    
}

extern Mtx gx_view;

/*
 * GX render callback
 */
void wii_render_callback()
{
  GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
  GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
  GX_SetVtxDesc( GX_VA_TEX0, GX_NONE );

  Mtx m;    // model matrix.
  Mtx mv;   // modelview matrix.

  guMtxIdentity( m ); 
  guMtxTransApply( m, m, 0, 0, -100 );
  guMtxConcat( gx_view, m, mv );
  GX_LoadPosMtxImm( mv, GX_PNMTX0 ); 

  // Diff switches
  wii_atari_display_diff_switches();

  //
  // Debug
  //

  static int dbg_count = 0;

  if( wii_debug && !wii_testframe )
  {    
    static char text[256] = "";
    static char text2[256] = "";
    dbg_count++;

    if( dbg_count % 60 == 0 )
    {
      /* a: %d, %d, c: 0x%x,0x%x,0x%x*/
      /* wii_sound_length, wii_convert_length, memory_ram[CTLSWB], riot_drb, memory_ram[SWCHB] */
      sprintf( text, 
        "v: %.2f, hs: %d, %d, timer: %d, wsync: %s, %d, stl: %s, mar: %d, cpu: %d, ext: %d, rnd: %d, hb: %d",
        wii_fps_counter, 
        high_score_set,
        hs_sram_write_count, 
        ( riot_timer_count % 1000 ),      
        ( dbg_wsync ? "1" : "0" ),
        dbg_wsync_count,
        ( dbg_cycle_stealing ? "1" : "0" ),            
        dbg_maria_cycles,
        dbg_p6502_cycles,
        dbg_saved_cycles,
        RANDOM,
        cartridge_hblank
      );       
    }

    //sprintf( text, "video: %.2f", wii_fps_counter );
    wii_gx_drawtext( -310, 210, 14, text, ftgxWhite, 0 ); 

    if( lightgun_enabled )
    {      
      //ir_t ir;
      //WPAD_IR(WPAD_CHAN_0, &ir);

      sprintf( text2, 
        "lightgun: %d, %d, %d, %.2f, %d, [%d, %d]", 
        /*"lightgun: %d, %d, %d, %.2f, %d, [%d, %d] %d, %d, %d, %d", */
        cartridge_crosshair_x, cartridge_crosshair_y,
        lightgun_scanline, lightgun_cycle, wii_dbg_scanlines, 
        wii_ir_x, wii_ir_y /*,
        ir.vres[0], ir.vres[1], ir.offset[0], ir.offset[1]*/ );

      wii_gx_drawtext( -310, -210, 14, text2, ftgxWhite, 0 );
    }
  }
}

/*
 * Runs the main Atari emulator loop
 *
 * testframes   The number of testframes to run (for loading saves)
 */
void wii_atari_main_loop( int testframes )
{
  WII_SetRenderCallback( &wii_render_callback );

  WII_ChangeSquare( wii_screen_x, wii_screen_y, 0, 0 );

  // Track the first fire of the lightgun (so that the catridge can properly
  // detect joystick versus lightgun.)
  lightgun_first_fire = true;

  // Only enable lightgun if the cartridge supports it and we are displaying
  // at 2x.
  lightgun_enabled = 
    ( cartridge_controller[0] & CARTRIDGE_CONTROLLER_LIGHTGUN );

  float fps_counter;
  u32 timerCount = 0;
  u32 start_time = SDL_GetTicks();

  timer_Reset();

  while( !prosystem_paused ) 
  {
    if( testframes < 0 )
    {
      wii_atari_update_keys( keyboard_data );
      wii_testframe = false;
    }
    else
    {
      wii_testframe = true;
    }

    if( prosystem_active && !prosystem_paused ) 
    {       
      prosystem_ExecuteFrame( keyboard_data );

      while( !timer_IsTime() );

      fps_counter = (((float)timerCount++/(SDL_GetTicks()-start_time))*1000.0);
      wii_atari_refresh_screen( true, testframes );

      if( testframes < 0 )
      {
        sound_Store();
      }

      wii_fps_counter = fps_counter;

      if( testframes > 0 )
      { 
        --testframes;
      }
      else if( testframes == 0 )
      {
        return;
      }
    }        
  }

  // Save the high score SRAM
  cartridge_SaveHighScoreSram();
}
 