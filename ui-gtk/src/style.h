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

/* List colors as Trello web paints them (Atlassian "subtler" accent tokens). The board gets
   .board-dark from the style manager: libadwaita's dark mode, whatever the system says. */
.column.list-green   { background: #baf3db; } .column.list-yellow  { background: #f8e6a0; }
.column.list-orange  { background: #fedec8; } .column.list-red     { background: #ffd5d2; }
.column.list-purple  { background: #dfd8fd; } .column.list-blue    { background: #cce0ff; }
.column.list-teal    { background: #c6edfb; } .column.list-lime    { background: #d3f1a7; }
.column.list-magenta { background: #fdd0ec; } .column.list-gray    { background: #dcdfe4; }
.column.list-colored { color: #172b4d; }
.board-dark .column.list-green   { background: #164b35; } .board-dark .column.list-yellow  { background: #533f04; }
.board-dark .column.list-orange  { background: #702e00; } .board-dark .column.list-red     { background: #5d1f1a; }
.board-dark .column.list-purple  { background: #352c63; } .board-dark .column.list-blue    { background: #09326c; }
.board-dark .column.list-teal    { background: #164555; } .board-dark .column.list-lime    { background: #37471f; }
.board-dark .column.list-magenta { background: #50253f; } .board-dark .column.list-gray    { background: #2c333a; }
.board-dark .column.list-colored { color: #dee4ea; }
/* Cards stay solid over a colored list (libadwaita's dark card color is translucent). */
.column.list-colored .card-row { background: #ffffff; color: #172b4d; }
.column.list-colored .card-row:hover { background: #f1f2f4; }
.board-dark .column.list-colored .card-row { background: #22272b; color: #dee4ea; }
.board-dark .column.list-colored .card-row:hover { background: #2c333a; }

/* Light board backgrounds (Trello's backgroundBrightness = light): dark scrollbars over them. */
.board-area.bg-light scrollbar slider { background: alpha(black, .45); }
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

.card-label { border-radius: 4px; padding: 0 6px; min-height: 16px;
              color: white; font-weight: 700; font-size: 0.75em; }
.card-label-empty { min-width: 40px; padding: 0; }
.card-row flowboxchild { padding: 0; min-width: 0; min-height: 0; }
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

.card-title { font-size: 1.4em; font-weight: 800; }
.title-edit { background: @card_bg_color; border-radius: 6px; padding: 2px 4px; }
.card-col > .section-title:first-child { margin-top: 0; }
.section-title { font-weight: 800; opacity: .8; margin-top: 6px; }
.comment-sending { opacity: .55; }
.comment-bubble { background: @card_bg_color; border-radius: 9px; padding: 8px 10px; }
.description-view { background: @card_bg_color; border-radius: 9px; padding: 6px; }

.thumb { border-radius: 8px; background: alpha(@view_fg_color, .08); }
.thumb-loading { background: alpha(@view_fg_color, .12); }
.thumb-failed { opacity: .5; }
.thumb-button { padding: 0; border-radius: 8px; }
.thumb-strip flowboxchild { padding: 0; min-width: 0; min-height: 0; }
.thumb-caption { font-size: smaller; opacity: .75; }

.description-rendered { border-radius: 9px; padding: 6px 8px; margin: 0 -8px; }
.description-rendered:hover { background: alpha(@view_fg_color, .04); }
.md-placeholder { font-style: italic; }
.markdown .md-h1 { font-size: 1.3em; font-weight: 800; margin-top: 4px; }
.markdown .md-h2 { font-size: 1.15em; font-weight: 800; margin-top: 4px; }
.markdown .md-h3 { font-weight: 800; }
.markdown .md-marker { opacity: .7; }
.markdown .md-quote { border-left: 3px solid alpha(@view_fg_color, .25); padding-left: 10px; }
.markdown .md-code { background: alpha(@view_fg_color, .07); border-radius: 6px; }
.markdown .md-code-text { font-family: monospace; padding: 6px 8px; }
.markdown .md-th { font-weight: 800; }
.markdown .card-link-chip { padding: 6px 10px; border-radius: 8px; background: alpha(@accent_bg_color, .12); }
.markdown .card-link-chip:hover { background: alpha(@accent_bg_color, .2); }

.status-offline { color: @warning_color; }
)css";

}  // namespace cardinal::gtk
