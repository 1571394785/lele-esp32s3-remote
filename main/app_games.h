#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_GAME_NONE = 0,
    APP_GAME_TETRIS,
    APP_GAME_SHOOTER,
    APP_GAME_BREAKOUT,
    APP_GAME_SNAKE,
} app_game_type_t;

typedef struct {
    app_game_type_t type;
    uint32_t last_tick_ms;
    uint32_t last_render_ms;
    uint32_t game_start_ms;
    uint32_t last_event_ms;
    uint16_t last_button_mask;
    bool running;
    bool game_over;

    uint8_t board[20][10];
    uint8_t piece;
    uint8_t rotation;
    int8_t piece_x;
    int8_t piece_y;
    uint16_t score;

    int8_t player_x;
    int8_t player_y;
    int8_t bullet_x[4];
    int8_t bullet_y[4];
    int8_t enemy_bullet_x[8];
    int8_t enemy_bullet_y[8];
    int8_t enemy_x[8];
    int8_t enemy_y[8];
    uint8_t enemy_alive[8];
    uint8_t enemy_step;

    uint8_t bricks[14][8];
    int8_t ball_x;
    int8_t ball_y;
    int8_t ball_dx;
    int8_t ball_dy;
    bool ball_launched;

    uint8_t snake_x[128];
    uint8_t snake_y[128];
    uint8_t snake_length;
    int8_t snake_dx;
    int8_t snake_dy;
    uint8_t food_x;
    uint8_t food_y;
} app_game_state_t;

void app_games_init(app_game_state_t *state);
void app_games_start(app_game_state_t *state, app_game_type_t type, uint32_t now_ms);
bool app_games_is_running(const app_game_state_t *state);
esp_err_t app_games_update(app_game_state_t *state, uint16_t button_mask, uint32_t now_ms, bool *exit_requested);
