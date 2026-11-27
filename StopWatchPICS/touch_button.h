#pragma once

struct TouchButton {
  int x,y,w,h;
  constexpr bool hit(int tx,int ty) const {
    return tx>=x && ty>=y && tx<x+w && ty<y+h;
  }
};
constexpr TouchButton home_weather_btn{46,119,376,42};
constexpr TouchButton home_usage_btn{122,333,224,100};
constexpr TouchButton display_trial_btn{80,360,308,48};
constexpr TouchButton weather_wifi_btn{112,344,244,48};
constexpr TouchButton alarm_stop_btn{134,293,200,52};
constexpr TouchButton alarm_hours_minus_btn{65,197,76,48};
constexpr TouchButton alarm_hours_plus_btn{148,197,76,48};
constexpr TouchButton alarm_minutes_minus_btn{244,197,76,48};
constexpr TouchButton alarm_minutes_plus_btn{327,197,76,48};
constexpr TouchButton alarm_on_btn{81,269,188,48};
constexpr TouchButton alarm_preview_btn{281,269,107,48};
