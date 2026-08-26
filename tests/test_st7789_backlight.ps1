$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\st7789_driver.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\st7789_driver.h"
$pins = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\board_pins.h"
$mainCmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$manifest = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\idf_component.yml"
$sdkconfig = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"
$uiAssets = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.c"
$expected = 'gpio_set_level\(LUMIDOCK_LCD_BL_GPIO, 0\)'

if ($driver -notmatch $expected) {
    throw 'ST7789 backlight must be held low while the panel is initialized.'
}

$backlightSet = $driver.IndexOf('gpio_set_level(LUMIDOCK_LCD_BL_GPIO, 0)')
$spiInit = $driver.IndexOf('spi_bus_initialize')
if ($backlightSet -lt 0 -or $backlightSet -gt $spiInit) {
    throw 'ST7789 must disable the backlight before starting SPI initialization.'
}
if ($driver -notmatch 'held LOW until the LVGL startup frame is ready') {
    throw 'ST7789 must report that the backlight is held off until LVGL has a startup frame.'
}
if ($driver -notmatch 'gpio_set_direction\(LUMIDOCK_LCD_BL_GPIO, GPIO_MODE_INPUT_OUTPUT\)') {
    throw 'ST7789 must enable the backlight input path before reading its pad level.'
}
if ($driver -notmatch 'ledc_fade_func_install' -or
    $driver -notmatch 'ledc_set_fade_with_time' -or
    $header -notmatch 'st7789_fade_backlight_percent') {
    throw 'Backlight transitions must use the LEDC hardware fade service.'
}

@(
    @{ Content = $pins; Pattern = '#define LUMIDOCK_WS2812_1_GPIO GPIO_NUM_13'; Message = 'S3-Zero external WS2812 must use GPIO13.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_BCLK_GPIO GPIO_NUM_10'; Message = 'S3-Zero I2S BCLK must use GPIO10.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_LRCLK_GPIO GPIO_NUM_9'; Message = 'S3-Zero I2S LRCLK must use GPIO9.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_MIC_DATA_GPIO GPIO_NUM_8'; Message = 'S3-Zero I2S microphone data must use GPIO8.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_LCD_SCLK_GPIO GPIO_NUM_1'; Message = 'S3-Zero TFT SCL must use GPIO1.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_LCD_MOSI_GPIO GPIO_NUM_2'; Message = 'S3-Zero TFT SDA must use GPIO2.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_LCD_RST_GPIO GPIO_NUM_11'; Message = 'S3-Zero TFT RES must use GPIO11.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_LCD_DC_GPIO GPIO_NUM_4'; Message = 'S3-Zero TFT DC must use GPIO4.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_LCD_CS_GPIO GPIO_NUM_5'; Message = 'S3-Zero TFT CS must use GPIO5.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_LCD_BL_GPIO GPIO_NUM_6'; Message = 'S3-Zero TFT BLK must use GPIO6.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_LCD_SPI_CLOCK_HZ \(30 \* 1000 \* 1000\)'; Message = 'The current LCD animation profile requires a 30 MHz SPI clock.' },
    @{ Content = $driver; Pattern = '\.mode = 3'; Message = 'ST7789 requires SPI mode 3 to match the vendor clock timing.' },
    @{ Content = $driver; Pattern = 'const uint8_t c5\[\] = \{0x1a\}'; Message = 'ST7789 must use the vendor VCOM setting.' },
    @{ Content = $driver; Pattern = 'const uint8_t orientation = 0x60'; Message = 'ST7789 main UI must use landscape orientation.' },
    @{ Content = $driver; Pattern = '#define ST7789_FILL_STRIPE_ROWS 8U'; Message = 'ST7789 full-screen clears must use 8-line SPI stripes.' },
    @{ Content = $driver; Pattern = 'static uint8_t s_fill_buffer\[ST7789_WIDTH \* ST7789_FILL_STRIPE_ROWS \* 2U\]'; Message = 'ST7789 full-screen clears must use an 8-line persistent buffer.' },
    @{ Content = $driver; Pattern = '\.max_transfer_sz = ST7789_WIDTH \* ST7789_FILL_STRIPE_ROWS \* 2'; Message = 'ST7789 SPI transfer size must be limited to one stripe.' },
    @{ Content = $driver; Pattern = 'for \(int y = 0; y < ST7789_HEIGHT; y \+= ST7789_FILL_STRIPE_ROWS\)'; Message = 'ST7789 full-screen fill must send the panel in stripes.' },
    @{ Content = $driver; Pattern = 'controller auto-increments the address window'; Message = 'Bitmap flushes must set the LCD window once before sending DMA stripes.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'static void create_startup_page'; Message = 'The display must create a startup page rather than replaying a bench test.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'lv_obj_add_flag\(s_scene\.startup_root, LV_OBJ_FLAG_HIDDEN\)'; Message = 'The startup page must transition to the main page without a full-screen hardware clear.' },
    @{ Content = $header; Pattern = 'st7789_draw_bitmap'; Message = 'The ST7789 driver must expose a bounded bitmap flush API for LVGL.' },
    @{ Content = $driver; Pattern = 'void st7789_draw_bitmap'; Message = 'The ST7789 driver must implement the LVGL bitmap flush API.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = '#include "lvgl.h"'; Message = 'The main UI must use LVGL rather than the temporary pixel-font renderer.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'lv_display_create\(ST7789_WIDTH, ST7789_HEIGHT\)'; Message = 'The LVGL display must use the verified 320x170 landscape geometry.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'LV_DISPLAY_RENDER_MODE_PARTIAL'; Message = 'LVGL must use partial rendering so its output stays within the ST7789 stripe limit.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = '#define LUMIDOCK_LVGL_BUFFER_ROWS 8U'; Message = 'LVGL must render no more than one ST7789 stripe at a time.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'lv_tick_inc\(20\)'; Message = 'LVGL must advance its tick so startup and weather animation can run.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'UI_BOOT_MIN_DURATION_US'; Message = 'The startup page must use a fixed monotonic duration independent of time synchronization.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'elapsed_us >= UI_BOOT_MAX_DURATION_US'; Message = 'The startup page must leave after its elapsed monotonic duration.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'static void create_status_bar'; Message = 'The pages must create a shared-style Wi-Fi, Bluetooth, and battery status bar.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'ui_icon_wifi'; Message = 'Wi-Fi must use a Flash-backed open-source icon asset.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'ui_icon_bluetooth'; Message = 'Bluetooth must use a Flash-backed open-source icon asset.' },
    @{ Content = (Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"); Pattern = 'lv_obj_set_pos\(s_scene\.main_weather_layer, 0, 0\)'; Message = 'The weather scene must cover the full page behind the status bar.' },
    @{ Content = $manifest; Pattern = 'lvgl/lvgl'; Message = 'LVGL must be declared as a managed component dependency.' },
    @{ Content = $sdkconfig; Pattern = 'CONFIG_LV_FONT_MONTSERRAT_48=y'; Message = 'The large LVGL time font must be enabled in the active project configuration.' },
    @{ Content = $driver; Pattern = 'const uint8_t vcom\[\] = \{0x3f\}'; Message = 'ST7789 must use the vendor VCOM setting.' },
    @{ Content = $driver; Pattern = 'write_command_data\(0xe8'; Message = 'ST7789 must use the vendor E8 power setting.' },
    @{ Content = $header; Pattern = '#define ST7789_WIDTH 320'; Message = 'ST7789 UI must use the 320-pixel landscape width.' },
    @{ Content = $header; Pattern = '#define ST7789_HEIGHT 170'; Message = 'ST7789 UI must use the 170-pixel landscape height.' },
    @{ Content = $header; Pattern = '#define ST7789_X_OFFSET 0'; Message = 'ST7789 landscape must use a zero column offset.' },
    @{ Content = $header; Pattern = '#define ST7789_Y_OFFSET 35'; Message = 'ST7789 landscape must use the vendor row offset.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($uiAssets -notmatch 'ui_main_scene' -or $uiAssets -notmatch 'ui_weather_clear') {
    throw 'The reference background and weather images must be available as Flash-backed LVGL assets.'
}
if ((Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c") -notmatch 'lv_image_set_src') {
    throw 'The main UI must render Flash-backed image assets rather than temporary weather shapes.'
}

if ((Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c") -match 'internet_probe_failures') {
    throw 'The startup page must not skip its animation after a fixed number of probe failures.'
}

if ($mainCmake -match 'startup_animation\.c') {
    throw 'The former fixed-duration startup animation module must not be compiled with the LVGL startup page.'
}

