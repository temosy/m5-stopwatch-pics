// Included inside the app's UI namespace after line() and the weather painter.
enum class WifiPage : uint8_t { Closed, List, Scan, Edit, Keyboard, Delete };
enum class WifiField : uint8_t { SSID, Password, Coordinates, Label };
WifiPage wifi_page=WifiPage::Closed;
WifiField wifi_field=WifiField::SSID;
wifi_profiles::Profile wifi_draft;
int wifi_slot=-1, wifi_scan_page=0, wifi_keys=0;
bool wifi_need_password=false, wifi_ui_dirty=false, wifi_deferred_scan=false;
char wifi_buffer[65]{};
const char* wifi_notice="";
local_weather::Survey::State wifi_drawn_scan=local_weather::Survey::Idle;
bool wifi_drawn_busy=false;

void wifi_discard() {
  wifi_profiles::wipe(&wifi_draft,sizeof(wifi_draft));
  wifi_draft=wifi_profiles::Profile{};
  wifi_profiles::wipe(wifi_buffer,sizeof(wifi_buffer));
}
void wifi_go(WifiPage page,const char* note="") {
  wifi_page=page; wifi_notice=note; wifi_ui_dirty=true;
}
void wifi_close() {
  wifi_discard(); wifi_deferred_scan=false; wifi_go(WifiPage::Closed);
}
void wifi_edit(int slot) {
  wifi_discard();
  wifi_slot=slot;
  wifi_need_password=wifi_book.entries[slot].cfg.password[0]!=0;
  wifi_draft=wifi_book.entries[slot].cfg.ssid[0] ? wifi_book.entries[slot] : wifi_profiles::Profile{};
  wifi_go(WifiPage::Edit);
}
void wifi_scan_now() {
  wifi_scan_page=0;
  wifi_go(WifiPage::Scan);
  wifi_deferred_scan=wifi_busy();
  if (!wifi_deferred_scan) local_weather::scan_start();
}
bool wifi_hit(int x,int y,int l,int t,int w,int h) {
  return TouchButton{l,t,w,h}.hit(x,y);
}
void wifi_tile(int x,int y,int w,int h,const char* text,uint16_t color=fg) {
  draw_button({x,y,w,h},text,color);
}
void wifi_keyboard(WifiField field) {
  wifi_field=field; wifi_keys=0;
  wifi_profiles::wipe(wifi_buffer,sizeof(wifi_buffer));
  switch(field) {
    case WifiField::SSID: snprintf(wifi_buffer,sizeof(wifi_buffer),"%s",wifi_draft.cfg.ssid);break;
    case WifiField::Password: break; // Never reveal the previous password.
    case WifiField::Coordinates:
      if(wifi_profiles::has_location(wifi_draft.cfg))
        snprintf(wifi_buffer,sizeof(wifi_buffer),"%.2f,%.2f",wifi_draft.cfg.lat/100.0,wifi_draft.cfg.lon/100.0);
      break;
    case WifiField::Label: break;
  }
  wifi_go(WifiPage::Keyboard);
}
const char* wifi_keyset() {
  if(wifi_field==WifiField::Coordinates) return "1234567890.,-";
  if(wifi_keys==0) return "qwertyuiopasdfghjklzxcvbnm1234567890";
  if(wifi_keys==1) return "QWERTYUIOPASDFGHJKLZXCVBNM1234567890";
  return "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~ ";
}
void wifi_keyboard_apply() {
  const size_t n=strlen(wifi_buffer);
  bool ok=true;
  switch(wifi_field) {
    case WifiField::SSID:
      ok=n && n<=32;
      if(ok && strcmp(wifi_draft.cfg.ssid,wifi_buffer)!=0) {
        wifi_profiles::wipe(wifi_draft.cfg.password,sizeof(wifi_draft.cfg.password));
        memcpy(wifi_draft.cfg.ssid,wifi_buffer,n+1);
      }
      break;
    case WifiField::Password:
      ok=(!n && !wifi_need_password) || (n>=8 && n<=63);
      if(ok) {
        wifi_profiles::wipe(wifi_draft.cfg.password,sizeof(wifi_draft.cfg.password));
        memcpy(wifi_draft.cfg.password,wifi_buffer,n+1);
      }
      break;
    case WifiField::Coordinates:
      ok=wifi_profiles::coordinates(wifi_buffer,wifi_draft.cfg.lat,wifi_draft.cfg.lon);
      break;
    case WifiField::Label:
      ok=n && n<=32;
      if(ok) memcpy(wifi_draft.cfg.label,wifi_buffer,n+1);
      break;
  }
  if(!ok) {
    wifi_notice=wifi_field==WifiField::Password ? "パスワードは8〜63文字" :
                wifi_field==WifiField::Coordinates ? "緯度,経度で入力" : "1〜32文字で入力";
    wifi_ui_dirty=true; return;
  }
  wifi_profiles::wipe(wifi_buffer,sizeof(wifi_buffer));
  wifi_go(WifiPage::Edit);
}
void draw_wifi_menu() {
  auto& d=M5.Display;
  const auto survey=local_weather::scan_read();
  wifi_drawn_scan=survey.status; wifi_drawn_busy=wifi_busy();
  wifi_ui_dirty=false;
  d.startWrite(); d.fillScreen(bg);
  auto title=[&](const char* name) {
    d.setFont(ui_fonts::heading.get());
    line(59,name,1,0x86ff);
    d.setFont(ui_fonts::body.get());
  };
  if(wifi_page==WifiPage::List) {
    title("Wi-Fi設定");
    char count[44];
    snprintf(count,sizeof(count),"登録 %d / %d",wifi_profiles::count(wifi_book),wifi_profiles::capacity);
    line(93,count,1,muted);
    for(int i=0;i<wifi_profiles::capacity;++i) {
      const auto& p=wifi_book.entries[i];
      wifi_tile(75,112+i*43,318,38,p.cfg.ssid[0] ? p.cfg.ssid : "未登録",p.cfg.ssid[0] ? fg : muted);
    }
    wifi_tile(77,340,151,39,"近くのWi-Fi",0x86ff);
    wifi_tile(240,340,151,39,"手動で追加",0xaff5);
  } else if(wifi_page==WifiPage::Scan) {
    title("近くのWi-Fi");
    if(wifi_deferred_scan) line(195,"天気の取得後に検索",1,muted);
    else if(survey.status==local_weather::Survey::Busy) line(195,"検索中…",1,muted);
    else if(survey.status==local_weather::Survey::Failed) line(195,"検索できませんでした",1,0xfaaa);
    else if(!survey.count) line(195,"Wi-Fiが見つかりません",1,muted);
    else {
      char pages[24];
      snprintf(pages,sizeof(pages),"%d / %d",wifi_scan_page+1,(survey.count+4)/5);
      line(93,pages,1,muted);
      for(int i=0;i<5 && i+wifi_scan_page*5<survey.count;++i)
        wifi_tile(75,112+i*43,318,38,survey.entries[i+wifi_scan_page*5].ssid);
    }
    wifi_tile(77,340,98,39,"前へ",0x86ff);
    wifi_tile(185,340,98,39,"再検索",0x86ff);
    wifi_tile(293,340,98,39,"次へ",0x86ff);
  } else if(wifi_page==WifiPage::Edit) {
    title("登録情報");
    wifi_tile(77,96,314,40,wifi_draft.cfg.ssid[0] ? wifi_draft.cfg.ssid : "Wi-Fi名を入力");
    wifi_tile(77,142,314,40,wifi_draft.cfg.password[0] ? "パスワード：登録あり" : "パスワードを入力");
    wifi_tile(77,188,314,40,wifi_draft.mobile ? "移動用：基本地点を使用" : "固定地点：このWi-Fiの天気");
    if(!wifi_draft.mobile) {
      wifi_tile(77,235,151,40,wifi_draft.cfg.label,0x86ff);
      char xy[30]="緯度,経度を入力";
      if(wifi_profiles::has_location(wifi_draft.cfg))
        snprintf(xy,sizeof(xy),"%.2f,%.2f",wifi_draft.cfg.lat/100.0,wifi_draft.cfg.lon/100.0);
      wifi_tile(240,235,151,40,xy,0x86ff);
    }
    wifi_tile(77,340,151,39,"削除",0xfaaa);
    wifi_tile(240,340,151,39,"保存",0xaff5);
  } else if(wifi_page==WifiPage::Keyboard) {
    title(wifi_field==WifiField::Password ? "パスワード" : wifi_field==WifiField::Coordinates ? "緯度,経度" :
          wifi_field==WifiField::SSID ? "Wi-Fi名" : "地点名");
    char shown[65];
    if(wifi_field==WifiField::Password) {
      const size_t n=strlen(wifi_buffer);
      snprintf(shown,sizeof(shown),"%u文字  ",unsigned(n));
      size_t off=strlen(shown);
      for(size_t i=0;i<std::min(n,size_t(18));++i) shown[off++]='*';
      shown[off]=0;
    } else snprintf(shown,sizeof(shown),"%s",wifi_buffer[0] ? wifi_buffer : "入力してください");
    wifi_tile(62,91,344,42,shown,fg);
    const char* keys=wifi_keyset();
    for(int i=0;keys[i];++i) {
      char glyph[2]={keys[i],0};
      wifi_tile(58+44*(i%8),146+37*(i/8),40,33,keys[i]==' ' ? "SP" : glyph);
    }
    wifi_tile(77,335,98,38,wifi_field==WifiField::Coordinates ? "クリア" : wifi_keys==0 ? "ABC" : wifi_keys==1 ? "記号" : "abc",0x86ff);
    wifi_tile(185,335,98,38,"消す",0xffc8);
    wifi_tile(293,335,98,38,"決定",0xaff5);
  } else if(wifi_page==WifiPage::Delete) {
    title("Wi-Fiを削除");
    wifi_tile(77,165,314,48,wifi_draft.cfg.ssid);
    d.setFont(ui_fonts::body.get());
    line(257,"この登録を削除しますか？",1,fg);
    wifi_tile(77,340,151,39,"キャンセル",0x86ff);
    wifi_tile(240,340,151,39,"削除する",0xfaaa);
  }
  d.setFont(ui_fonts::body.get());
  if(wifi_notice[0]) line(399,wifi_notice,1,0xfaaa);
  else line(404,"黄：戻る",1,0xffc8);
  d.setFont(&lgfx::fonts::Font0); d.endWrite();
}
void wifi_back() {
  if(wifi_page==WifiPage::List) { wifi_close(); start_weather("wifi-settings"); }
  else if(wifi_page==WifiPage::Keyboard) {
    wifi_profiles::wipe(wifi_buffer,sizeof(wifi_buffer));
    wifi_go(WifiPage::Edit);
  } else if(wifi_page==WifiPage::Delete) wifi_go(WifiPage::Edit);
  else { wifi_discard(); wifi_deferred_scan=false; wifi_go(WifiPage::List); }
}
void wifi_tap(int x,int y) {
  auto hit=[&](int l,int t,int w,int h){return wifi_hit(x,y,l,t,w,h);};
  const auto survey=local_weather::scan_read();
  if(wifi_page==WifiPage::List) {
    for(int i=0;i<wifi_profiles::capacity;++i)
      if(hit(75,112+i*43,318,38)) {
        const bool existing=wifi_book.entries[i].cfg.ssid[0];
        wifi_edit(i);
        if(!existing) wifi_keyboard(WifiField::SSID);
        return;
      }
    if(hit(77,340,151,39)) wifi_scan_now();
    if(hit(240,340,151,39)) {
      const int slot=wifi_profiles::empty(wifi_book);
      if(slot<0) wifi_go(WifiPage::List,"登録は5件まで");
      else { wifi_edit(slot); wifi_keyboard(WifiField::SSID); }
    }
  } else if(wifi_page==WifiPage::Scan) {
    if(hit(77,340,98,39) && wifi_scan_page>0) { --wifi_scan_page; wifi_ui_dirty=true; }
    if(hit(185,340,98,39)) wifi_scan_now();
    if(hit(293,340,98,39) && (wifi_scan_page+1)*5<survey.count) { ++wifi_scan_page; wifi_ui_dirty=true; }
    if(wifi_deferred_scan || survey.status!=local_weather::Survey::Ready) return;
    for(int i=0;i<5 && i+wifi_scan_page*5<survey.count;++i) if(hit(75,112+i*43,318,38)) {
      const auto& row=survey.entries[i+wifi_scan_page*5];
      const int existing=wifi_profiles::find(wifi_book,row.ssid);
      const int slot=existing>=0 ? existing : wifi_profiles::empty(wifi_book);
      if(slot<0) { wifi_go(WifiPage::Scan,"登録は5件まで"); return; }
      wifi_edit(slot);
      memcpy(wifi_draft.cfg.ssid,row.ssid,sizeof(row.ssid));
      wifi_need_password=row.secured;
      if(existing<0 && row.secured) wifi_keyboard(WifiField::Password);
      return;
    }
  } else if(wifi_page==WifiPage::Edit) {
    if(hit(77,96,314,40)) wifi_keyboard(WifiField::SSID);
    else if(hit(77,142,314,40)) wifi_keyboard(WifiField::Password);
    else if(hit(77,188,314,40)) { wifi_draft.mobile=!wifi_draft.mobile; wifi_ui_dirty=true; }
    else if(!wifi_draft.mobile && hit(77,235,151,40)) wifi_keyboard(WifiField::Label);
    else if(!wifi_draft.mobile && hit(240,235,151,40)) wifi_keyboard(WifiField::Coordinates);
    else if(hit(77,340,151,39) && wifi_book.entries[wifi_slot].cfg.ssid[0]) wifi_go(WifiPage::Delete);
    else if(hit(240,340,151,39)) {
      auto next=wifi_book;
      const int existing=wifi_profiles::find(next,wifi_draft.cfg.ssid);
      if (existing>=0 && existing!=wifi_slot) wifi_go(WifiPage::Edit,"同じWi-Fiは登録済み");
      else if(wifi_need_password && !wifi_draft.cfg.password[0]) wifi_go(WifiPage::Edit,"パスワードを入力");
      else if(!wifi_profiles::set(next,wifi_slot,wifi_draft)) wifi_go(WifiPage::Edit,"登録情報を確認");
      else if(!save_wifi_book(next)) wifi_go(WifiPage::Edit,wifi_busy() || local_weather::scanning() ? "Wi-Fi処理中です" : "保存できませんでした");
      else { wifi_discard(); wifi_go(WifiPage::List,"保存しました"); }
      wifi_profiles::wipe(&next,sizeof(next));
    }
  } else if(wifi_page==WifiPage::Keyboard) {
    const char* keys=wifi_keyset();
    const size_t n=strlen(wifi_buffer);
    const size_t limit=wifi_field==WifiField::Password ? 63 : 32;
    for(int i=0;keys[i];++i) if(hit(58+44*(i%8),146+37*(i/8),40,33)) {
      if(n<limit) { wifi_buffer[n]=keys[i]; wifi_buffer[n+1]=0; wifi_ui_dirty=true; }
      return;
    }
    if(hit(77,335,98,38)) {
      if(wifi_field==WifiField::Coordinates) wifi_profiles::wipe(wifi_buffer,sizeof(wifi_buffer));
      else wifi_keys=(wifi_keys+1)%3;
      wifi_ui_dirty=true;
    } else if(hit(185,335,98,38) && n) {
      size_t i=n; do { wifi_buffer[--i]=0; }
      while(i && (static_cast<uint8_t>(wifi_buffer[i-1])&0xc0)==0x80);
      if(i && (static_cast<uint8_t>(wifi_buffer[i-1])&0xc0)==0xc0) wifi_buffer[i-1]=0;
      wifi_ui_dirty=true;
    } else if(hit(293,335,98,38)) wifi_keyboard_apply();
  } else if(wifi_page==WifiPage::Delete) {
    if(hit(77,340,151,39)) wifi_go(WifiPage::Edit);
    else if(hit(240,340,151,39)) {
      auto next=wifi_book;
      next.entries[wifi_slot]=wifi_profiles::Profile{};
      if(save_wifi_book(next)) { wifi_discard(); wifi_go(WifiPage::List,"削除しました"); }
      else wifi_go(WifiPage::Delete,"削除できませんでした");
      wifi_profiles::wipe(&next,sizeof(next));
    }
  }
}
