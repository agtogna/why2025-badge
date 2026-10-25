#include "snake.h"
#include <inttypes.h>

const char ui_snake_eyes[][4] = {"' '", ": ", ". .", " :"};

static snake_t snake;

// ---- high score, persisted to a SPIFFS file ----
static uint32_t snake_load_high(void)
{
    uint32_t v = 0;
    FILE *fp = fopen(SNAKE_HIGHSCORE_FILE, "r");
    if (fp) {
        if (fscanf(fp, "%" SCNu32, &v) != 1) v = 0;
        fclose(fp);
    }
    return v;
}

static void snake_save_high(uint32_t v)
{
    FILE *fp = fopen(SNAKE_HIGHSCORE_FILE, "w");
    if (fp) {
        fprintf(fp, "%u", (unsigned)v);
        fclose(fp);
    }
}

// keep the HUD labels drawn above the snake body/food
static void snake_labels_front(void)
{
    if (snake.lbl_score) lv_obj_move_foreground(snake.lbl_score);
    if (snake.lbl_high)  lv_obj_move_foreground(snake.lbl_high);
}

static void snake_update_labels(void)
{
    lv_label_set_text_fmt(snake.lbl_score, "SCORE %d", snake.score);
    lv_label_set_text_fmt(snake.lbl_high,  "HIGH %u", (unsigned)snake.high);
    // re-anchor top-right so a growing number doesn't clip off the edge
    lv_obj_align(snake.lbl_high, LV_ALIGN_TOP_RIGHT, -4, 4);
    snake_labels_front();
}

void snake_set_dir(int8_t dir)
{
    dir += snake.dir;
    if (dir < SNAKE_DIR_BEGIN)
        dir = SNAKE_DIR_END;
    if (dir > SNAKE_DIR_END)
        dir = SNAKE_DIR_BEGIN;
    snake.dir = dir;
    lv_label_set_text(snake.eye, ui_snake_eyes[snake.dir - SNAKE_DIR_BEGIN]);
}

static void snake_add_body(lv_obj_t *parent)
{
    if (snake.size >= UI_SNAKE_MAX_BODY)
        return;

    int index = snake.size++;
    snake.body[index] = lv_button_create(parent);
    lv_obj_add_state(snake.body[index], LV_STATE_CHECKED);
    lv_obj_set_size(snake.body[index], UI_SNAKE_BODY_SIZE, UI_SNAKE_BODY_SIZE);
}

void snake_reset(lv_obj_t *parent)
{
    if(snake_task_handle)
        lv_timer_pause(snake_task_handle);

    srand(lv_tick_get());

    // high score: load once at boot. snake.high already tracks the live max
    // (the eat handler bumps it), so persist it UNCONDITIONALLY here — gating on
    // score>high would never fire because high was already raised to == score
    // during play, which is why the record wasn't being saved. Saving the loaded
    // value at boot is a harmless no-op; snake.high is never below the stored one.
    static bool hi_loaded = false;
    if (!hi_loaded) { snake.high = snake_load_high(); hi_loaded = true; }
    if (snake.score > (int)snake.high) snake.high = snake.score;
    snake_save_high(snake.high);
    snake.score = 0;

    // HUD labels: black text (white screen), top corners.
    // Created once (snake_reset is also the death handler and doesn't clear them).
    if (!snake.lbl_score) {
        snake.lbl_score = lv_label_create(parent);
        lv_obj_set_style_text_color(snake.lbl_score, lv_color_black(), 0);
        lv_obj_align(snake.lbl_score, LV_ALIGN_TOP_LEFT, 4, 4);
    }
    if (!snake.lbl_high) {
        snake.lbl_high = lv_label_create(parent);
        lv_obj_set_style_text_color(snake.lbl_high, lv_color_black(), 0);
        lv_obj_align(snake.lbl_high, LV_ALIGN_TOP_RIGHT, -4, 4);
    }

    for (int i = 0; i < snake.size; i++)
    {
        lv_obj_delete(snake.body[i]);
        snake.body[i] = NULL;
    }
    snake.size = 0;
    if (snake.food)
    {
        lv_obj_delete(snake.food);
        snake.food = NULL;
    }

    snake_add_body(parent); // head
    snake.eye = lv_label_create(snake.body[0]);
    snake_set_dir(SNAKE_DIR_RIGHT);
    lv_obj_set_pos(snake.body[0], UI_SNAKE_BODY_SIZE * 3, UI_SNAKE_BODY_SIZE * 6);

    snake_add_body(parent);
    lv_obj_set_pos(snake.body[1], UI_SNAKE_BODY_SIZE * 2, UI_SNAKE_BODY_SIZE * 6);
    snake_add_body(parent);
    lv_obj_set_pos(snake.body[2], UI_SNAKE_BODY_SIZE * 1, UI_SNAKE_BODY_SIZE * 6);

    snake.speed = SNAKE_MIN_SPEED;
    snake_update_labels();
}

static int counter = 0;

void snake_task(lv_timer_t *arg)
{
    // not current page, ignore.
    if (lv_screen_active() != screen_snake){
        lv_timer_pause(snake_task_handle);
        return;
    }

    if(counter < snake.speed) {
        counter++;
        return;
    }
    
    counter = 0;

    lv_obj_t *parent = screen_snake;
    lv_obj_update_layout(parent); // v9 lv_obj_get_x() is stale until layout runs
    int32_t x = lv_obj_get_x(snake.body[0]);
    int32_t y = lv_obj_get_y(snake.body[0]);

    switch (snake.dir)
    {
        case SNAKE_DIR_UP:
            y -= UI_SNAKE_BODY_SIZE;
            break;
        case SNAKE_DIR_DOWN:
            y += UI_SNAKE_BODY_SIZE;
            break;
        case SNAKE_DIR_LEFT:
            x -= UI_SNAKE_BODY_SIZE;
            break;
        case SNAKE_DIR_RIGHT:
            x += UI_SNAKE_BODY_SIZE;
            break;
    }

    // check if head hit any wall.
    if (x >= LV_HOR_RES || x < 0 || y >= LV_VER_RES || y < 0)
    {
        snake_reset(screen_snake);
        return;
    }
    // check if head hit any body.
    for (int i = 1; i < snake.size; i++)
    {
        if (x == lv_obj_get_x(snake.body[i]) && y == lv_obj_get_y(snake.body[i]))
        {
            snake_reset(screen_snake);
            return;
        }
    }
    // check if head hit any food.
    if (snake.food)
    {
        if (x == lv_obj_get_x(snake.food) && y == lv_obj_get_y(snake.food))
        {
            lv_obj_delete(snake.food);
            snake.food = NULL;
            snake_add_body(parent);

            snake.score++;                 // one point per ball eaten
            if (snake.score > (int)snake.high) snake.high = snake.score;
            snake_update_labels();

            if(snake.size % SNAKE_STEP_SPEED == 0 && snake.speed > 0){
                snake.speed--;
                ESP_LOGI(__FILE__, "Snake speed is: %d", snake.speed);
            }
        }
    }
    // if no food exists, we need to generate one in random place.
    else
    {
        snake.food = lv_button_create(screen_snake);
        lv_obj_set_size(snake.food, UI_SNAKE_BODY_SIZE, UI_SNAKE_BODY_SIZE);
        lv_obj_set_pos(snake.food,
                       (rand() % LV_HOR_RES) / UI_SNAKE_BODY_SIZE * UI_SNAKE_BODY_SIZE,
                       (rand() % LV_VER_RES) / UI_SNAKE_BODY_SIZE * UI_SNAKE_BODY_SIZE);
        snake_labels_front();   // keep the HUD above the freshly spawned food
    }

    // now we can move the body.
    for (int i = snake.size - 1; i >= 1; i--)
    {
        int32_t cx = lv_obj_get_x(snake.body[i - 1]);
        int32_t cy = lv_obj_get_y(snake.body[i - 1]);
        lv_obj_set_pos(snake.body[i], cx, cy);
    }
    lv_obj_set_pos(snake.body[0], x, y); // head is last one.
}