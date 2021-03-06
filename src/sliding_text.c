#include <pebble.h>
#include <ctype.h>
#include "num2words.h"

#define BUFF_SIZE 32

typedef enum {
  MOVING_IN,
  IN_FRAME,
  PREPARE_TO_MOVE,
  MOVING_OUT
} SlideState;

typedef struct {
  TextLayer *label;
  SlideState state; // animation state
  char *next_string; // what to say in the next phase of animation
  bool unchanged_font;

  int left_pos;
  int right_pos;
  int still_pos;

  int movement_delay;
  int delay_count;
} SlidingRow;

typedef struct {
  SlidingRow rows[3];
  int last_hour;
  int last_minute;
  
  TextLayer *steps_label;
  char steps_text[BUFF_SIZE];
  
  TextLayer *date_label;
  char date_text[BUFF_SIZE];
  

  GFont bitham42_bold;
  GFont bitham42_light;

  Window *window;
  Animation *animation;

  struct SlidingTextRenderState {
    // double buffered string storage
    char hours[2][32];
    uint8_t next_hours;
    char first_minutes[2][32];
    char second_minutes[2][32];
    uint8_t next_minutes;
  } render_state;

} SlidingTextData;

SlidingTextData *s_data;

static void init_sliding_row(SlidingTextData *data, SlidingRow *row, GRect pos, GFont font,
        int delay) {
  row->label = text_layer_create(pos);
  text_layer_set_text_alignment(row->label, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  text_layer_set_background_color(row->label, GColorClear);
  text_layer_set_text_color(row->label, GColorWhite);
  if (font) {
    text_layer_set_font(row->label, font);
    row->unchanged_font = true;
  } else {
    row->unchanged_font = false;
  }

  row->state = IN_FRAME;
  row->next_string = NULL;

  row->left_pos = -pos.size.w;
  row->right_pos = pos.size.w;
  row->still_pos = pos.origin.x;

  row->movement_delay = delay;
  row->delay_count = 0;

  data->last_hour = -1;
  data->last_minute = -1;
}

static void slide_in_text(SlidingTextData *data, SlidingRow *row, char* new_text) {
  (void) data;

  const char *old_text = text_layer_get_text(row->label);
  if (old_text) {
    row->next_string = new_text;
    row->state = PREPARE_TO_MOVE;
  } else {
    text_layer_set_text(row->label, new_text);
    GRect frame = layer_get_frame(text_layer_get_layer(row->label));
    frame.origin.x = row->right_pos;
    layer_set_frame(text_layer_get_layer(row->label), frame);
    row->state = MOVING_IN;
  }
}


static bool update_sliding_row(SlidingTextData *data, SlidingRow *row) {
  (void) data;

  GRect frame = layer_get_frame(text_layer_get_layer(row->label));
  bool something_changed = true;
  switch (row->state) {
    case PREPARE_TO_MOVE:
      frame.origin.x = row->still_pos;
      row->delay_count++;
      if (row->delay_count > row->movement_delay) {
        row->state = MOVING_OUT;
        row->delay_count = 0;
      }
    break;

    case MOVING_IN: {
      int speed = abs(frame.origin.x - row->still_pos) / 3 + 1;
      frame.origin.x -= speed;
      if (frame.origin.x <= row->still_pos) {
        frame.origin.x = row->still_pos;
        row->state = IN_FRAME;
      }
    }
    break;

    case MOVING_OUT: {
      int speed = abs(frame.origin.x - row->still_pos) / 3 + 1;
      frame.origin.x -= speed;

      if (frame.origin.x <= row->left_pos) {
        frame.origin.x = row->right_pos;
        row->state = MOVING_IN;
        text_layer_set_text(row->label, row->next_string);
        row->next_string = NULL;
      }
    }
    break;

    case IN_FRAME:
    default:
      something_changed = false;
      break;
  }
  if (something_changed) {
    layer_set_frame(text_layer_get_layer(row->label), frame);
  }
  return something_changed;
}

static void animation_update(struct Animation *animation, const AnimationProgress time_normalized) {
  SlidingTextData *data = s_data;

  struct SlidingTextRenderState *rs = &data->render_state;

  time_t now = time(NULL);
  struct tm t = *localtime(&now);

  bool something_changed = false;

  if (data->last_minute != t.tm_min) {
    something_changed = true;

    minute_to_formal_words(t.tm_min, rs->first_minutes[rs->next_minutes], rs->second_minutes[rs->next_minutes]);
    if(data->last_hour != t.tm_hour || t.tm_min <= 20
       || t.tm_min/10 != data->last_minute/10) {
      slide_in_text(data, &data->rows[1], rs->first_minutes[rs->next_minutes]);
    } else {
      // The tens line didn't change, so swap to the correct buffer but don't animate
      text_layer_set_text(data->rows[1].label, rs->first_minutes[rs->next_minutes]);
    }
    slide_in_text(data, &data->rows[2], rs->second_minutes[rs->next_minutes]);
    rs->next_minutes = rs->next_minutes ? 0 : 1;
    data->last_minute = t.tm_min;
  }

  if (data->last_hour != t.tm_hour) {
    hour_to_12h_word(t.tm_hour, rs->hours[rs->next_hours]);
    slide_in_text(data, &data->rows[0], rs->hours[rs->next_hours]);
    rs->next_hours = rs->next_hours ? 0 : 1;
    data->last_hour = t.tm_hour;
  }

  for (size_t i = 0; i < ARRAY_LENGTH(data->rows); ++i) {
    something_changed = update_sliding_row(data, &data->rows[i]) || something_changed;
  }

  if (!something_changed) {
    animation_unschedule(data->animation);
  }
}

static void make_animation() {
  s_data->animation = animation_create();
  animation_set_duration(s_data->animation, ANIMATION_DURATION_INFINITE);
                  // the animation will stop itself
  static const struct AnimationImplementation s_animation_implementation = {
    .update = animation_update,
  };
  animation_set_implementation(s_data->animation, &s_animation_implementation);
  animation_schedule(s_data->animation);
}

static void handle_minute_tick(struct tm *tick_time, TimeUnits units_changed) {
  make_animation();
  
  if (s_data) {
    int steps = (int) health_service_sum_today(HealthMetricStepCount);
    #if defined(PBL_RECT)
    if (steps == 1) {
      snprintf(s_data->steps_text, BUFF_SIZE, "%d step", steps);
    }
    else {
      snprintf(s_data->steps_text, BUFF_SIZE, "%d steps", steps);
    }
    #elif defined(PBL_ROUND)
    snprintf(s_data->steps_text, BUFF_SIZE, "%d", steps);
    #endif
    text_layer_set_text(s_data->steps_label, s_data->steps_text);
    
    // The Date
    if (tick_time->tm_mday == 1 || tick_time->tm_mday == 21 || tick_time->tm_mday == 31) {
      if (tick_time->tm_mday < 10) {
        strftime(s_data->date_text, BUFF_SIZE, "%A the%est", tick_time);
      }
      else {
        strftime(s_data->date_text, BUFF_SIZE, "%A the %dst", tick_time);
      }
    }
    else if (tick_time->tm_mday == 2 || tick_time->tm_mday == 22) {
      if (tick_time->tm_mday < 10) {
        strftime(s_data->date_text, BUFF_SIZE, "%A the%end", tick_time);
      }
      else {
        strftime(s_data->date_text, BUFF_SIZE, "%A the %dnd", tick_time);
      }
    }
    else if (tick_time->tm_mday == 3 || tick_time->tm_mday == 23) {
      if (tick_time->tm_mday < 10) {
        strftime(s_data->date_text, BUFF_SIZE, "%A the%erd", tick_time);
      }
      else {
        strftime(s_data->date_text, BUFF_SIZE, "%A the %drd", tick_time);
      }    }
    else {
      if (tick_time->tm_mday < 10) {
        strftime(s_data->date_text, BUFF_SIZE, "%A the%eth", tick_time);
      }
      else {
        strftime(s_data->date_text, BUFF_SIZE, "%A the %dth", tick_time);
      }
    } 
    text_layer_set_text(s_data->date_label, s_data->date_text);
  }
}

static void handle_deinit(void) {
  tick_timer_service_unsubscribe();
  free(s_data);
}

static void handle_init() {
  SlidingTextData *data = (SlidingTextData*)malloc(sizeof(SlidingTextData));
  s_data = data;

  data->render_state.next_hours = 0;
  data->render_state.next_minutes = 0;
  data->window = window_create();

  window_set_background_color(data->window, GColorBlack);

  data->bitham42_bold = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  data->bitham42_light = fonts_get_system_font(FONT_KEY_BITHAM_42_LIGHT);

  Layer *window_layer = window_get_root_layer(data->window);
  GRect layer_frame = layer_get_frame(window_layer);
  const int16_t width = layer_frame.size.w;
  const int16_t height = layer_frame.size.h;
  
  #if defined(PBL_RECT)
  init_sliding_row(data, &data->rows[0], GRect(0, 6, width, 60), data->bitham42_bold, 6);
  #elif defined(PBL_ROUND)
  init_sliding_row(data, &data->rows[0], GRect(0, 14, width, 60), data->bitham42_bold, 6);
  #endif
  layer_add_child(window_layer, text_layer_get_layer(data->rows[0].label));

  #if defined(PBL_RECT)
  init_sliding_row(data, &data->rows[1], GRect(0, 42, width, 96), data->bitham42_light, 3);
  #elif defined(PBL_ROUND)
  init_sliding_row(data, &data->rows[1], GRect(0, 50, width, 96), data->bitham42_light, 3);
  #endif
  layer_add_child(window_layer, text_layer_get_layer(data->rows[1].label));

  #if defined(PBL_RECT)
  init_sliding_row(data, &data->rows[2], GRect(0, 78, width, 132), data->bitham42_light, 0);
  #elif defined(PBL_ROUND)
  init_sliding_row(data, &data->rows[2], GRect(0, 86, width, 132), data->bitham42_light, 0);
  #endif
  layer_add_child(window_layer, text_layer_get_layer(data->rows[2].label));

  GFont norm14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GFont bold14 = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  GFont norm18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont bold18 = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
   
  #if defined(PBL_RECT)
  data->steps_label = text_layer_create(GRect(0, height - 38, width-3, 20));
  text_layer_set_text_alignment(data->steps_label, GTextAlignmentRight);
  text_layer_set_font(data->steps_label, bold18);
  #elif defined(PBL_ROUND)
  data->steps_label = text_layer_create(GRect(0, height - 27, width, 20));
  text_layer_set_text_alignment(data->steps_label, GTextAlignmentCenter);
  text_layer_set_font(data->steps_label, bold14);
  #endif
  
  text_layer_set_background_color(data->steps_label, GColorClear);
  text_layer_set_text_color(data->steps_label, GColorWhite);
  text_layer_set_text(data->steps_label, "");
  layer_add_child(window_layer, text_layer_get_layer(data->steps_label));
  
  #if defined(PBL_RECT)
  data->date_label = text_layer_create(GRect(0, height - 22, width-3, 20));
  text_layer_set_text_alignment(data->date_label, GTextAlignmentRight);
  text_layer_set_font(data->date_label, norm18);
  #elif defined(PBL_ROUND)
  data->date_label = text_layer_create(GRect(0, height - 42, width, 20));
  text_layer_set_text_alignment(data->date_label, GTextAlignmentCenter);
  text_layer_set_font(data->date_label, norm14);
  #endif
  
  text_layer_set_background_color(data->date_label, GColorClear);
  text_layer_set_text_color(data->date_label, GColorWhite);
  text_layer_set_text(data->date_label, "");
  layer_add_child(window_layer, text_layer_get_layer(data->date_label));
  
  layer_mark_dirty(window_layer);

  make_animation();

  tick_timer_service_subscribe(MINUTE_UNIT, handle_minute_tick);

  const bool animated = true;
  window_stack_push(data->window, animated);
}

int main(void) {
  handle_init();

  app_event_loop();

  handle_deinit();
}

