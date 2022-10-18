
#include "lv_conf.h"
#include <lvgl.h>

#define LV_TICK_PERIOD_MS 1
    
/*** Setup screen resolution for LVGL ***/
static const uint16_t screenWidth = TFT_WIDTH;
static const uint16_t screenHeight = TFT_HEIGHT;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 20];
static lv_color_t buf2[screenWidth * 20];

static lv_disp_t *disp;
static lv_theme_t *theme_current;
static lv_color_t bg_theme_color;

static LGFX lcd; // declare display variable

/* Creates a semaphore to handle concurrent call to lvgl stuff
 * If you wish to call *any* lvgl function from other threads/tasks
 * you should lock on the very same semaphore! */
static SemaphoreHandle_t xGuiSemaphore = NULL;
static TaskHandle_t g_lvgl_task_handle;

static void gui_task(void *args);

/*** Function declaration ***/
void display_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);

void touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data);

static void lv_tick_task(void *arg);

esp_err_t lv_display_init()
{
    esp_err_t ret;
    // Setting display to landscape
    // if (lcd.width() < lcd.height()) lcd.setRotation(lcd.getRotation() ^ 2);

    lcd.setBrightness(128);
    lcd.setColorDepth(24);

    lcd.fillScreen(TFT_BLACK);

    /* LVGL : Setting up buffer to use for display */
    lv_disp_draw_buf_init(&draw_buf, buf, buf2, screenWidth * 20);

    /*** LVGL : Setup & Initialize the display device driver ***/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = display_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.sw_rotate = 1;
    disp = lv_disp_drv_register(&disp_drv);

    //*** LVGL : Setup & Initialize the input device driver ***
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&indev_drv);

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t lv_periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui"};
    esp_timer_handle_t lv_periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&lv_periodic_timer_args, &lv_periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lv_periodic_timer, LV_TICK_PERIOD_MS * 1000));

    // Setup theme
    theme_current = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                          lv_palette_main(LV_PALETTE_RED),
                                          LV_USE_THEME_DEFAULT, /*Light or dark mode*/
                                          &lv_font_montserrat_14);

    // lv_disp_set_theme(disp, th); /*Assign the theme to the display*/
    bg_theme_color = theme_current->flags & LV_USE_THEME_DEFAULT ? lv_palette_darken(LV_PALETTE_GREY, 4) : lv_palette_lighten(LV_PALETTE_GREY, 1);

    xGuiSemaphore = xSemaphoreCreateMutex();
    if (!xGuiSemaphore)  
    {
        ESP_LOGE(TAG, "Create mutex for LVGL failed");
        if (lv_periodic_timer) esp_timer_delete(lv_periodic_timer);
        return ESP_FAIL;
    }

#if CONFIG_FREERTOS_UNICORE == 0
    int err = xTaskCreatePinnedToCore(gui_task, "lv gui", 1024 * 8, NULL, 5, &g_lvgl_task_handle, 1);
#else
    int err = xTaskCreatePinnedToCore(gui_task, "lv gui", 1024 * 8, NULL, 5, &g_lvgl_task_handle, 0);
#endif
    if (!err)
    {
        ESP_LOGE(TAG, "Create task for LVGL failed");
        if (lv_periodic_timer) esp_timer_delete(lv_periodic_timer);
        return ESP_FAIL;
    }

    esp_timer_start_periodic(lv_periodic_timer, LV_TICK_PERIOD_MS * 1000U);
    return ESP_OK;
}

// Display callback to flush the buffer to screen
void display_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.pushPixels((uint16_t *)&color_p->full, w * h, true);
    lcd.endWrite();

    lv_disp_flush_ready(disp);
}

/* Setting up tick task for lvgl */
static void lv_tick_task(void *arg)
{
    (void)arg;
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

static void gui_task(void *args)
{
    ESP_LOGI(TAG, "Start to run LVGL");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            lv_task_handler();
            //lv_timer_handler_run_in_period(5); /* run lv_timer_handler() every 5ms */
            xSemaphoreGive(xGuiSemaphore);
        }
    }
}

void lvgl_acquire(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (g_lvgl_task_handle != task) {
        xSemaphoreTake(xGuiSemaphore, portMAX_DELAY);
    }
}

void lvgl_release(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (g_lvgl_task_handle != task) {
        xSemaphoreGive(xGuiSemaphore);
    }
}

// Touchpad callback to read the touchpad
void touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    uint16_t touchX, touchY;
    bool touched = lcd.getTouch(&touchX, &touchY);

    if (!touched)
    {
        data->state = LV_INDEV_STATE_REL;
    }
    else
    {
        data->state = LV_INDEV_STATE_PR;

        // Set the coordinates
        data->point.x = touchX;
        data->point.y = touchY;
    }
}
