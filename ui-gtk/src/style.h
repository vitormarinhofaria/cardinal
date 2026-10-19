#pragma once

namespace cardinal::gtk {

// Loaded once for the default display. Colors come from libadwaita's named colors so light and
// dark styles both work.
inline const char* kStyleCss = R"css(
.board-area {
  background: linear-gradient(135deg, #0b6fb3, #4f5fc4);
}
.board-area scrollbar slider { background: alpha(white, .55); }

.column {
  background: alpha(@window_bg_color, .96);
  border-radius: 14px;
  padding: 8px;
  min-width: 272px;
}
.column listview, .column scrolledwindow { background: transparent; }
.column-title { font-weight: 800; }
.column-count { opacity: .6; font-size: smaller; }
.column-cold { padding: 24px 8px; }

.card-row {
  background: @card_bg_color;
  border-radius: 9px;
  padding: 8px 10px;
  margin: 3px 2px;
  box-shadow: 0 1px 1px alpha(black, .22), 0 0 0 1px alpha(black, .04);
}
.card-row:hover { background: mix(@card_bg_color, @accent_bg_color, .09); }
.card-row.drop-target { box-shadow: 0 -3px 0 0 @accent_bg_color; }
.card-meta { opacity: .7; font-size: smaller; }
.card-row avatar { margin-left: 2px; }
.add-card { padding: 4px 8px; }
.column.drop-target { box-shadow: inset 0 0 0 2px @accent_bg_color; }

.label-bar { border-radius: 4px; min-height: 8px; min-width: 40px; }
.label-chip { border-radius: 4px; padding: 1px 8px; color: white; font-weight: 700; font-size: smaller; }
.lbl-green  { background: #61bd4f; } .lbl-yellow { background: #c9ad00; color: #1d2125; }
.lbl-orange { background: #e88d00; } .lbl-red    { background: #eb5a46; }
.lbl-purple { background: #c377e0; } .lbl-blue   { background: #0079bf; }
.lbl-sky    { background: #00a2c0; } .lbl-lime   { background: #2fbf7a; }
.lbl-pink   { background: #e050a8; } .lbl-black  { background: #344563; }
.lbl-none   { background: #8590a2; }
.lbl-green_light  { background: #baf3db; color: #1d2125; } .lbl-green_dark  { background: #1f845a; }
.lbl-yellow_light { background: #f8e6a0; color: #1d2125; } .lbl-yellow_dark { background: #946f00; }
.lbl-orange_light { background: #fedec8; color: #1d2125; } .lbl-orange_dark { background: #c25100; }
.lbl-red_light    { background: #ffd5d2; color: #1d2125; } .lbl-red_dark    { background: #c9372c; }
.lbl-purple_light { background: #dfd8fd; color: #1d2125; } .lbl-purple_dark { background: #6e5dc6; }
.lbl-blue_light   { background: #cce0ff; color: #1d2125; } .lbl-blue_dark   { background: #0c66e4; }
.lbl-sky_light    { background: #c6edfb; color: #1d2125; } .lbl-sky_dark    { background: #227d9b; }
.lbl-lime_light   { background: #d3f1a7; color: #1d2125; } .lbl-lime_dark   { background: #5b7f24; }
.lbl-pink_light   { background: #fdd0ec; color: #1d2125; } .lbl-pink_dark   { background: #ae4787; }
.lbl-black_light  { background: #dcdfe4; color: #1d2125; } .lbl-black_dark  { background: #44546f; }

.panel { background: @window_bg_color; }
.panel-title { font-size: 1.25em; font-weight: 800; }
.section-title { font-weight: 800; opacity: .8; margin-top: 6px; }
.comment-sending { opacity: .55; }
.comment-bubble { background: @card_bg_color; border-radius: 9px; padding: 8px 10px; }
.description-view { background: @card_bg_color; border-radius: 9px; padding: 6px; }

.status-offline { color: @warning_color; }
)css";

}  // namespace cardinal::gtk
