#include "app_games.h"

#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "esp_check.h"
#include "esp_random.h"
#include "ssd1306.h"

#define TETRIS_W 10
#define TETRIS_H 20
#define TETRIS_CELL 5
#define GAME_FIELD_W 64
#define GAME_FIELD_H 128
#define TETRIS_X 7
#define TETRIS_Y 24
#define SHOOTER_ENEMIES 8
#define SHOOTER_BULLETS 4
#define SHOOTER_ENEMY_BULLETS 8
#define BREAKOUT_ROWS 14
#define BREAKOUT_INITIAL_ROWS 5
#define BREAKOUT_COLS 8
#define BREAKOUT_BRICK_W 7
#define BREAKOUT_BRICK_H 4
#define SNAKE_COLS 12
#define SNAKE_ROWS 21
#define SNAKE_CELL 5
#define SNAKE_X 2
#define SNAKE_Y 20
#define SNAKE_MAX_LENGTH 128

static const int8_t TETRIS_SHAPES[7][4][4][2] = {
    {{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}}, {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}},
    {{{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}},
    {{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}},
    {{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}}, {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}},
    {{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}},
    {{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}},
    {{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}},
};

static bool just_pressed(uint16_t mask, uint16_t last_mask, uint16_t key_mask)
{
    return (mask & key_mask) != 0 && (last_mask & key_mask) == 0;
}

static uint16_t vertical_game_mask(uint16_t mask)
{
    uint16_t mapped = mask & (BUTTON_MASK_FUNC1 | BUTTON_MASK_FUNC2 | BUTTON_MASK_FUNC3 | BUTTON_MASK_FUNC4);

    if (mask & BUTTON_MASK_RIGHT) mapped |= BUTTON_MASK_UP;
    if (mask & BUTTON_MASK_LEFT) mapped |= BUTTON_MASK_DOWN;
    if (mask & BUTTON_MASK_UP) mapped |= BUTTON_MASK_LEFT;
    if (mask & BUTTON_MASK_DOWN) mapped |= BUTTON_MASK_RIGHT;
    return mapped;
}

void app_games_init(app_game_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

static bool tetris_collides(const app_game_state_t *state, int8_t x, int8_t y, uint8_t rotation)
{
    for (uint8_t i = 0; i < 4; i++) {
        int8_t px = x + TETRIS_SHAPES[state->piece][rotation][i][0];
        int8_t py = y + TETRIS_SHAPES[state->piece][rotation][i][1];
        if (px < 0 || px >= TETRIS_W || py >= TETRIS_H) {
            return true;
        }
        if (py >= 0 && state->board[py][px] != 0) {
            return true;
        }
    }
    return false;
}

static void tetris_spawn(app_game_state_t *state)
{
    state->piece = esp_random() % 7;
    state->rotation = 0;
    state->piece_x = 3;
    state->piece_y = -1;
    if (tetris_collides(state, state->piece_x, state->piece_y, state->rotation)) {
        state->game_over = true;
    }
}

static void tetris_lock_piece(app_game_state_t *state)
{
    for (uint8_t i = 0; i < 4; i++) {
        int8_t px = state->piece_x + TETRIS_SHAPES[state->piece][state->rotation][i][0];
        int8_t py = state->piece_y + TETRIS_SHAPES[state->piece][state->rotation][i][1];
        if (px >= 0 && px < TETRIS_W && py >= 0 && py < TETRIS_H) {
            state->board[py][px] = 1;
        }
    }

    for (int8_t y = TETRIS_H - 1; y >= 0; y--) {
        bool full = true;
        for (uint8_t x = 0; x < TETRIS_W; x++) {
            if (state->board[y][x] == 0) {
                full = false;
                break;
            }
        }
        if (full) {
            for (int8_t yy = y; yy > 0; yy--) {
                memcpy(state->board[yy], state->board[yy - 1], sizeof(state->board[yy]));
            }
            memset(state->board[0], 0, sizeof(state->board[0]));
            state->score += 10;
            y++;
        }
    }
    tetris_spawn(state);
}

static void tetris_draw_border(void)
{
    int x0 = TETRIS_X - 1;
    int y0 = TETRIS_Y - 1;
    int x1 = TETRIS_X + TETRIS_W * TETRIS_CELL;
    int y1 = TETRIS_Y + TETRIS_H * TETRIS_CELL;

    for (int x = x0; x <= x1; x++) {
        ssd1306_draw_pixel_ccw(x, y0, true);
        ssd1306_draw_pixel_ccw(x, y1, true);
    }
    for (int y = y0; y <= y1; y++) {
        ssd1306_draw_pixel_ccw(x0, y, true);
        ssd1306_draw_pixel_ccw(x1, y, true);
    }
}

static void tetris_step_down(app_game_state_t *state)
{
    if (!tetris_collides(state, state->piece_x, state->piece_y + 1, state->rotation)) {
        state->piece_y++;
    } else {
        tetris_lock_piece(state);
    }
}

static void tetris_render(const app_game_state_t *state)
{
    char score[16];

    ssd1306_clear();
    ssd1306_draw_text16_ccw(2, 0, "TETRIS");
    snprintf(score, sizeof(score), "%u", state->score);
    ssd1306_draw_text16_ccw(92, 0, score);
    tetris_draw_border();

    for (uint8_t y = 0; y < TETRIS_H; y++) {
        for (uint8_t x = 0; x < TETRIS_W; x++) {
            if (state->board[y][x] != 0) {
                ssd1306_fill_rect_ccw(TETRIS_X + x * TETRIS_CELL, TETRIS_Y + y * TETRIS_CELL,
                                      TETRIS_CELL - 1, TETRIS_CELL - 1, true);
            }
        }
    }
    for (uint8_t i = 0; i < 4; i++) {
        int8_t px = state->piece_x + TETRIS_SHAPES[state->piece][state->rotation][i][0];
        int8_t py = state->piece_y + TETRIS_SHAPES[state->piece][state->rotation][i][1];
        if (py >= 0) {
            ssd1306_fill_rect_ccw(TETRIS_X + px * TETRIS_CELL, TETRIS_Y + py * TETRIS_CELL,
                                  TETRIS_CELL - 1, TETRIS_CELL - 1, true);
        }
    }
    if (state->game_over) {
        ssd1306_draw_text16_ccw(38, 24, "GAME");
        ssd1306_draw_text16_ccw(72, 24, "OVER");
    }
    ssd1306_flush();
}

static void shooter_spawn_enemies(app_game_state_t *state)
{
    for (uint8_t i = 0; i < SHOOTER_ENEMIES; i++) {
        state->enemy_x[i] = 10 + (i % 4) * 14;
        state->enemy_y[i] = 16 + (i / 4) * 12;
        state->enemy_alive[i] = 1;
    }
}

static void breakout_reset(app_game_state_t *state)
{
    for (uint8_t y = 0; y < BREAKOUT_ROWS; y++) {
        for (uint8_t x = 0; x < BREAKOUT_COLS; x++) {
            state->bricks[y][x] = y < BREAKOUT_INITIAL_ROWS;
        }
    }
    state->player_x = GAME_FIELD_W / 2;
    state->ball_x = state->player_x;
    state->ball_y = 106;
    state->ball_dx = 1;
    state->ball_dy = -2;
    state->ball_launched = false;
}

static void breakout_push_bricks(app_game_state_t *state)
{
    for (uint8_t col = 0; col < BREAKOUT_COLS; col++) {
        if (state->bricks[BREAKOUT_ROWS - 1][col]) {
            state->game_over = true;
            return;
        }
    }

    for (int row = BREAKOUT_ROWS - 1; row > 0; row--) {
        memcpy(state->bricks[row], state->bricks[row - 1], sizeof(state->bricks[row]));
    }
    for (uint8_t col = 0; col < BREAKOUT_COLS; col++) {
        state->bricks[0][col] = (esp_random() % 100) < 72;
    }
}

static int8_t breakout_lowest_occupied_row(const app_game_state_t *state)
{
    for (int8_t row = BREAKOUT_ROWS - 1; row >= 0; row--) {
        for (uint8_t col = 0; col < BREAKOUT_COLS; col++) {
            if (state->bricks[row][col]) {
                return row;
            }
        }
    }
    return -1;
}

static bool breakout_row_is_empty(const app_game_state_t *state, uint8_t row)
{
    for (uint8_t col = 0; col < BREAKOUT_COLS; col++) {
        if (state->bricks[row][col]) {
            return false;
        }
    }
    return true;
}

static bool snake_cell_occupied(const app_game_state_t *state, uint8_t x, uint8_t y)
{
    for (uint8_t i = 0; i < state->snake_length; i++) {
        if (state->snake_x[i] == x && state->snake_y[i] == y) {
            return true;
        }
    }
    return false;
}

static void snake_place_food(app_game_state_t *state)
{
    for (uint16_t attempt = 0; attempt < 512; attempt++) {
        uint8_t x = esp_random() % SNAKE_COLS;
        uint8_t y = esp_random() % SNAKE_ROWS;
        if (!snake_cell_occupied(state, x, y)) {
            state->food_x = x;
            state->food_y = y;
            return;
        }
    }
}

static void snake_reset(app_game_state_t *state)
{
    state->snake_length = 4;
    state->snake_dx = 1;
    state->snake_dy = 0;
    for (uint8_t i = 0; i < state->snake_length; i++) {
        state->snake_x[i] = 5 - i;
        state->snake_y[i] = 10;
    }
    snake_place_food(state);
}

void app_games_start(app_game_state_t *state, app_game_type_t type, uint32_t now_ms)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->type = type;
    state->running = true;
    state->last_tick_ms = now_ms;
    state->last_render_ms = 0;
    state->game_start_ms = now_ms;
    state->last_event_ms = now_ms;
    state->last_button_mask = 0;

    if (type == APP_GAME_TETRIS) {
        tetris_spawn(state);
    } else if (type == APP_GAME_SHOOTER) {
        state->player_x = GAME_FIELD_W / 2;
        state->player_y = 116;
        for (uint8_t i = 0; i < SHOOTER_BULLETS; i++) {
            state->bullet_y[i] = -1;
        }
        for (uint8_t i = 0; i < SHOOTER_ENEMY_BULLETS; i++) {
            state->enemy_bullet_y[i] = -1;
        }
        shooter_spawn_enemies(state);
    } else if (type == APP_GAME_BREAKOUT) {
        breakout_reset(state);
    } else if (type == APP_GAME_SNAKE) {
        snake_reset(state);
    }
}

bool app_games_is_running(const app_game_state_t *state)
{
    return state != NULL && state->running;
}

static void tetris_update(app_game_state_t *state, uint16_t mask, uint16_t last_mask, uint32_t now_ms)
{
    if (state->game_over) {
        if (just_pressed(mask, last_mask, BUTTON_MASK_FUNC1)) {
            app_games_start(state, APP_GAME_TETRIS, now_ms);
        }
        return;
    }

    if (just_pressed(mask, last_mask, BUTTON_MASK_LEFT) &&
        !tetris_collides(state, state->piece_x - 1, state->piece_y, state->rotation)) {
        state->piece_x--;
    }
    if (just_pressed(mask, last_mask, BUTTON_MASK_RIGHT) &&
        !tetris_collides(state, state->piece_x + 1, state->piece_y, state->rotation)) {
        state->piece_x++;
    }
    if (just_pressed(mask, last_mask, BUTTON_MASK_UP)) {
        uint8_t next_rotation = (state->rotation + 1) & 0x03;
        if (!tetris_collides(state, state->piece_x, state->piece_y, next_rotation)) {
            state->rotation = next_rotation;
        }
    }
    if (just_pressed(mask, last_mask, BUTTON_MASK_DOWN)) {
        tetris_step_down(state);
    }
    if (just_pressed(mask, last_mask, BUTTON_MASK_FUNC1)) {
        while (!state->game_over && !tetris_collides(state, state->piece_x, state->piece_y + 1, state->rotation)) {
            state->piece_y++;
        }
        tetris_lock_piece(state);
    }
    if (now_ms - state->last_tick_ms >= 650) {
        tetris_step_down(state);
        state->last_tick_ms = now_ms;
    }
}

static void shooter_fire(app_game_state_t *state)
{
    for (uint8_t i = 0; i < SHOOTER_BULLETS; i++) {
        if (state->bullet_y[i] < 0) {
            state->bullet_x[i] = state->player_x;
            state->bullet_y[i] = state->player_y - 5;
            return;
        }
    }
}

static void shooter_enemy_fire(app_game_state_t *state)
{
    uint8_t start = esp_random() % SHOOTER_ENEMIES;
    for (uint8_t n = 0; n < SHOOTER_ENEMIES; n++) {
        uint8_t enemy = (start + n) % SHOOTER_ENEMIES;
        if (!state->enemy_alive[enemy]) continue;
        for (uint8_t b = 0; b < SHOOTER_ENEMY_BULLETS; b++) {
            if (state->enemy_bullet_y[b] < 0) {
                state->enemy_bullet_x[b] = state->enemy_x[enemy];
                state->enemy_bullet_y[b] = state->enemy_y[enemy] + 5;
                return;
            }
        }
    }
}

static void shooter_update(app_game_state_t *state, uint16_t mask, uint16_t last_mask, uint32_t now_ms)
{
    if (state->game_over) {
        if (just_pressed(mask, last_mask, BUTTON_MASK_FUNC1)) {
            app_games_start(state, APP_GAME_SHOOTER, now_ms);
        }
        return;
    }

    if ((mask & BUTTON_MASK_LEFT) && state->player_x > 4) state->player_x -= 2;
    if ((mask & BUTTON_MASK_RIGHT) && state->player_x < GAME_FIELD_W - 5) state->player_x += 2;
    if ((mask & BUTTON_MASK_UP) && state->player_y > 60) state->player_y -= 2;
    if ((mask & BUTTON_MASK_DOWN) && state->player_y < GAME_FIELD_H - 8) state->player_y += 2;
    if (just_pressed(mask, last_mask, BUTTON_MASK_FUNC1)) shooter_fire(state);

    uint32_t elapsed_s = (now_ms - state->game_start_ms) / 1000;
    uint32_t step_ms = elapsed_s >= 90 ? 30 : 65 - elapsed_s / 3;
    uint32_t fire_interval_ms = elapsed_s >= 90 ? 250 : 1150 - elapsed_s * 10;

    if (now_ms - state->last_event_ms >= fire_interval_ms) {
        shooter_enemy_fire(state);
        state->last_event_ms = now_ms;
    }

    if (now_ms - state->last_tick_ms < step_ms) {
        return;
    }
    state->last_tick_ms = now_ms;

    for (uint8_t i = 0; i < SHOOTER_BULLETS; i++) {
        if (state->bullet_y[i] >= 0) {
            state->bullet_y[i] -= 4;
            if (state->bullet_y[i] < 0) {
                state->bullet_y[i] = -1;
            }
        }
    }
    for (uint8_t i = 0; i < SHOOTER_ENEMY_BULLETS; i++) {
        if (state->enemy_bullet_y[i] >= 0) {
            int8_t bullet_speed = elapsed_s >= 60 ? 3 : 2;
            if (state->enemy_bullet_y[i] >= GAME_FIELD_H - bullet_speed) {
                state->enemy_bullet_y[i] = -1;
                continue;
            }
            state->enemy_bullet_y[i] += bullet_speed;
            if (state->enemy_bullet_x[i] >= state->player_x - 3 &&
                       state->enemy_bullet_x[i] <= state->player_x + 3 &&
                       state->enemy_bullet_y[i] >= state->player_y - 3 &&
                       state->enemy_bullet_y[i] <= state->player_y + 4) {
                state->game_over = true;
            }
        }
    }

    state->enemy_step++;
    int8_t dx = (state->enemy_step & 0x10) ? -1 : 1;
    uint8_t move_divisor = elapsed_s >= 75 ? 2 : elapsed_s >= 35 ? 4 : 7;
    if ((state->enemy_step % move_divisor) == 0) {
        for (uint8_t i = 0; i < SHOOTER_ENEMIES; i++) {
            if (state->enemy_alive[i]) {
                state->enemy_x[i] += dx;
                if (state->enemy_x[i] < 6) state->enemy_x[i] = 6;
                if (state->enemy_x[i] > GAME_FIELD_W - 6) state->enemy_x[i] = GAME_FIELD_W - 6;
                if ((state->enemy_step % (elapsed_s >= 60 ? 18 : 32)) == 0) state->enemy_y[i] += 2;
                if (state->enemy_y[i] > 108) state->game_over = true;
                if (state->enemy_x[i] >= state->player_x - 6 &&
                    state->enemy_x[i] <= state->player_x + 6 &&
                    state->enemy_y[i] >= state->player_y - 6 &&
                    state->enemy_y[i] <= state->player_y + 5) {
                    state->game_over = true;
                }
            }
        }
    }

    for (uint8_t b = 0; b < SHOOTER_BULLETS; b++) {
        if (state->bullet_y[b] < 0) continue;
        for (uint8_t e = 0; e < SHOOTER_ENEMIES; e++) {
            if (!state->enemy_alive[e]) continue;
            if (state->bullet_x[b] >= state->enemy_x[e] - 4 && state->bullet_x[b] <= state->enemy_x[e] + 4 &&
                state->bullet_y[b] >= state->enemy_y[e] - 3 && state->bullet_y[b] <= state->enemy_y[e] + 3) {
                state->enemy_alive[e] = 0;
                state->bullet_y[b] = -1;
                state->score += 5;
                break;
            }
        }
    }

    bool any_alive = false;
    for (uint8_t i = 0; i < SHOOTER_ENEMIES; i++) {
        any_alive |= state->enemy_alive[i] != 0;
    }
    if (!any_alive) {
        shooter_spawn_enemies(state);
    }
}

static void shooter_render(const app_game_state_t *state)
{
    char score[16];
    ssd1306_clear();
    snprintf(score, sizeof(score), "%u", state->score);
    ssd1306_draw_text16_ccw(2, 0, "THUNDER");
    ssd1306_draw_text16_ccw(96, 0, score);

    ssd1306_fill_rect_ccw(state->player_x - 1, state->player_y - 5, 3, 8, true);
    ssd1306_fill_rect_ccw(state->player_x - 5, state->player_y, 11, 3, true);
    ssd1306_draw_pixel_ccw(state->player_x - 4, state->player_y + 3, true);
    ssd1306_draw_pixel_ccw(state->player_x + 4, state->player_y + 3, true);

    for (uint8_t i = 0; i < SHOOTER_BULLETS; i++) {
        if (state->bullet_y[i] >= 0) {
            ssd1306_fill_rect_ccw(state->bullet_x[i], state->bullet_y[i], 1, 3, true);
        }
    }
    for (uint8_t i = 0; i < SHOOTER_ENEMY_BULLETS; i++) {
        if (state->enemy_bullet_y[i] >= 0) {
            ssd1306_fill_rect_ccw(state->enemy_bullet_x[i], state->enemy_bullet_y[i], 2, 3, true);
        }
    }
    for (uint8_t i = 0; i < SHOOTER_ENEMIES; i++) {
        if (state->enemy_alive[i]) {
            if ((i & 1) == 0) {
                ssd1306_fill_rect_ccw(state->enemy_x[i] - 4, state->enemy_y[i], 9, 3, true);
                ssd1306_fill_rect_ccw(state->enemy_x[i] - 2, state->enemy_y[i] - 2, 5, 7, true);
                ssd1306_draw_pixel_ccw(state->enemy_x[i] - 5, state->enemy_y[i] + 3, true);
                ssd1306_draw_pixel_ccw(state->enemy_x[i] + 5, state->enemy_y[i] + 3, true);
            } else {
                ssd1306_fill_rect_ccw(state->enemy_x[i] - 5, state->enemy_y[i], 11, 2, true);
                ssd1306_fill_rect_ccw(state->enemy_x[i] - 2, state->enemy_y[i] - 2, 5, 6, true);
                ssd1306_draw_pixel_ccw(state->enemy_x[i], state->enemy_y[i] + 4, true);
            }
        }
    }
    if (state->game_over) {
        ssd1306_draw_text16_ccw(48, 16, "GAME");
        ssd1306_draw_text16_ccw(66, 16, "OVER");
    }
    ssd1306_flush();
}

static void breakout_update(app_game_state_t *state, uint16_t mask, uint16_t last_mask, uint32_t now_ms)
{
    if (state->game_over) {
        if (just_pressed(mask, last_mask, BUTTON_MASK_FUNC1)) {
            app_games_start(state, APP_GAME_BREAKOUT, now_ms);
        }
        return;
    }

    if ((mask & BUTTON_MASK_LEFT) && state->player_x > 10) state->player_x -= 2;
    if ((mask & BUTTON_MASK_RIGHT) && state->player_x < GAME_FIELD_W - 10) state->player_x += 2;

    uint32_t elapsed_s = (now_ms - state->game_start_ms) / 1000;

    if (!state->ball_launched) {
        state->ball_x = state->player_x;
        state->ball_y = 106;
        if (just_pressed(mask, last_mask, BUTTON_MASK_FUNC1)) {
            state->ball_launched = true;
        }
        return;
    }

    uint32_t ball_step_ms = elapsed_s >= 100 ? 22 : 35 - elapsed_s / 8;
    if (now_ms - state->last_tick_ms < ball_step_ms) {
        return;
    }
    state->last_tick_ms = now_ms;

    state->ball_x += state->ball_dx;
    state->ball_y += state->ball_dy;

    if (state->ball_x <= 1) {
        state->ball_x = 1;
        state->ball_dx = 1;
    } else if (state->ball_x >= GAME_FIELD_W - 2) {
        state->ball_x = GAME_FIELD_W - 2;
        state->ball_dx = -1;
    }
    if (state->ball_y <= 1) {
        state->ball_y = 1;
        state->ball_dy = 2;
    }

    if (state->ball_y >= 104 && state->ball_y <= 108 &&
        state->ball_x >= state->player_x - 10 && state->ball_x <= state->player_x + 10) {
        state->ball_y = 103;
        state->ball_dy = -2;
        if (state->ball_x < state->player_x - 3) {
            state->ball_dx = -1;
        } else if (state->ball_x > state->player_x + 3) {
            state->ball_dx = 1;
        }
    }

    int8_t lowest_row = breakout_lowest_occupied_row(state);
    bool lowest_row_cleared = false;
    for (uint8_t row = 0; row < BREAKOUT_ROWS; row++) {
        for (uint8_t col = 0; col < BREAKOUT_COLS; col++) {
            if (!state->bricks[row][col]) continue;
            int bx = 3 + col * BREAKOUT_BRICK_W;
            int by = 18 + row * (BREAKOUT_BRICK_H + 2);
            if (state->ball_x >= bx && state->ball_x < bx + BREAKOUT_BRICK_W - 1 &&
                state->ball_y >= by && state->ball_y < by + BREAKOUT_BRICK_H) {
                state->bricks[row][col] = 0;
                state->ball_dy = -state->ball_dy;
                state->score += 5;
                lowest_row_cleared = row == lowest_row && breakout_row_is_empty(state, row);
                row = BREAKOUT_ROWS;
                break;
            }
        }
    }

    if (lowest_row_cleared) {
        breakout_push_bricks(state);
    }
    if (state->ball_y > GAME_FIELD_H - 8) {
        state->game_over = true;
    }
}

static void breakout_render(const app_game_state_t *state)
{
    char score[16];
    ssd1306_clear();
    ssd1306_draw_text16_ccw(2, 0, "BREAK");
    snprintf(score, sizeof(score), "%u", state->score);
    ssd1306_draw_text16_ccw(96, 0, score);

    for (uint8_t row = 0; row < BREAKOUT_ROWS; row++) {
        for (uint8_t col = 0; col < BREAKOUT_COLS; col++) {
            if (state->bricks[row][col]) {
                ssd1306_fill_rect_ccw(3 + col * BREAKOUT_BRICK_W,
                                      18 + row * (BREAKOUT_BRICK_H + 2),
                                      BREAKOUT_BRICK_W - 2, BREAKOUT_BRICK_H, true);
            }
        }
    }

    ssd1306_fill_rect_ccw(state->player_x - 10, 108, 21, 3, true);
    ssd1306_fill_rect_ccw(state->ball_x - 1, state->ball_y - 1, 3, 3, true);

    if (!state->ball_launched && !state->game_over) {
        ssd1306_draw_text16_ccw(28, 52, "F1");
    }
    if (state->game_over) {
        ssd1306_draw_text16_ccw(48, 16, "GAME");
        ssd1306_draw_text16_ccw(66, 16, "OVER");
    }
    ssd1306_flush();
}

static void snake_update(app_game_state_t *state, uint16_t mask, uint16_t last_mask, uint32_t now_ms)
{
    if (state->game_over) {
        if (just_pressed(mask, last_mask, BUTTON_MASK_FUNC1)) {
            app_games_start(state, APP_GAME_SNAKE, now_ms);
        }
        return;
    }

    if (just_pressed(mask, last_mask, BUTTON_MASK_UP) && state->snake_dy == 0) {
        state->snake_dx = 0;
        state->snake_dy = -1;
    } else if (just_pressed(mask, last_mask, BUTTON_MASK_DOWN) && state->snake_dy == 0) {
        state->snake_dx = 0;
        state->snake_dy = 1;
    } else if (just_pressed(mask, last_mask, BUTTON_MASK_LEFT) && state->snake_dx == 0) {
        state->snake_dx = -1;
        state->snake_dy = 0;
    } else if (just_pressed(mask, last_mask, BUTTON_MASK_RIGHT) && state->snake_dx == 0) {
        state->snake_dx = 1;
        state->snake_dy = 0;
    }

    uint32_t step_ms = state->score >= 60 ? 75 : 230 - state->score * 2;
    if (now_ms - state->last_tick_ms < step_ms) {
        return;
    }
    state->last_tick_ms = now_ms;

    int16_t next_x = state->snake_x[0] + state->snake_dx;
    int16_t next_y = state->snake_y[0] + state->snake_dy;
    if (next_x < 0) next_x = SNAKE_COLS - 1;
    else if (next_x >= SNAKE_COLS) next_x = 0;
    if (next_y < 0) next_y = SNAKE_ROWS - 1;
    else if (next_y >= SNAKE_ROWS) next_y = 0;

    bool eating = next_x == state->food_x && next_y == state->food_y;
    uint8_t collision_length = eating ? state->snake_length : state->snake_length - 1;
    for (uint8_t i = 0; i < collision_length; i++) {
        if (state->snake_x[i] == next_x && state->snake_y[i] == next_y) {
            state->game_over = true;
            return;
        }
    }

    uint8_t new_length = state->snake_length;
    if (eating && new_length < SNAKE_MAX_LENGTH) {
        new_length++;
    }
    for (uint8_t i = new_length - 1; i > 0; i--) {
        state->snake_x[i] = state->snake_x[i - 1];
        state->snake_y[i] = state->snake_y[i - 1];
    }
    state->snake_x[0] = (uint8_t)next_x;
    state->snake_y[0] = (uint8_t)next_y;
    state->snake_length = new_length;

    if (eating) {
        state->score++;
        snake_place_food(state);
    }
}

static void snake_render(const app_game_state_t *state)
{
    char score[24];
    ssd1306_clear();
    snprintf(score, sizeof(score), "SCORE %u", state->score);
    ssd1306_draw_text16_ccw(2, 0, score);

    for (uint8_t x = 0; x <= SNAKE_COLS * SNAKE_CELL; x++) {
        ssd1306_draw_pixel_ccw(SNAKE_X + x, SNAKE_Y - 1, true);
        ssd1306_draw_pixel_ccw(SNAKE_X + x, SNAKE_Y + SNAKE_ROWS * SNAKE_CELL, true);
    }
    for (uint8_t y = 0; y <= SNAKE_ROWS * SNAKE_CELL; y++) {
        ssd1306_draw_pixel_ccw(SNAKE_X - 1, SNAKE_Y + y, true);
        ssd1306_draw_pixel_ccw(SNAKE_X + SNAKE_COLS * SNAKE_CELL, SNAKE_Y + y, true);
    }

    ssd1306_fill_rect_ccw(SNAKE_X + state->food_x * SNAKE_CELL + 1,
                          SNAKE_Y + state->food_y * SNAKE_CELL + 1, 3, 3, true);
    for (uint8_t i = 0; i < state->snake_length; i++) {
        ssd1306_fill_rect_ccw(SNAKE_X + state->snake_x[i] * SNAKE_CELL,
                              SNAKE_Y + state->snake_y[i] * SNAKE_CELL,
                              SNAKE_CELL - 1, SNAKE_CELL - 1, true);
    }
    if (state->game_over) {
        ssd1306_draw_text16_ccw(48, 16, "GAME");
        ssd1306_draw_text16_ccw(66, 16, "OVER");
    }
    ssd1306_flush();
}

esp_err_t app_games_update(app_game_state_t *state, uint16_t button_mask, uint32_t now_ms, bool *exit_requested)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (exit_requested != NULL) {
        *exit_requested = false;
    }
    if (!state->running) {
        return ESP_OK;
    }
    uint16_t mapped_mask = vertical_game_mask(button_mask);
    uint16_t mapped_last_mask = vertical_game_mask(state->last_button_mask);

    if (just_pressed(mapped_mask, mapped_last_mask, BUTTON_MASK_FUNC2)) {
        state->running = false;
        if (exit_requested != NULL) {
            *exit_requested = true;
        }
        return ESP_OK;
    }

    if (state->type == APP_GAME_TETRIS) {
        tetris_update(state, mapped_mask, mapped_last_mask, now_ms);
        if (now_ms - state->last_render_ms >= 60 || mapped_mask != mapped_last_mask) {
            tetris_render(state);
            state->last_render_ms = now_ms;
        }
    } else if (state->type == APP_GAME_SHOOTER) {
        shooter_update(state, mapped_mask, mapped_last_mask, now_ms);
        if (now_ms - state->last_render_ms >= 40 || mapped_mask != mapped_last_mask) {
            shooter_render(state);
            state->last_render_ms = now_ms;
        }
    } else if (state->type == APP_GAME_BREAKOUT) {
        breakout_update(state, mapped_mask, mapped_last_mask, now_ms);
        if (now_ms - state->last_render_ms >= 35 || mapped_mask != mapped_last_mask) {
            breakout_render(state);
            state->last_render_ms = now_ms;
        }
    } else if (state->type == APP_GAME_SNAKE) {
        snake_update(state, mapped_mask, mapped_last_mask, now_ms);
        if (now_ms - state->last_render_ms >= 50 || mapped_mask != mapped_last_mask) {
            snake_render(state);
            state->last_render_ms = now_ms;
        }
    }

    state->last_button_mask = button_mask;
    return ESP_OK;
}
