let status = "Streaming test URL";

globalThis.init = function () {
  status = "Streaming test URL";
};

globalThis.tick = function () {
  host_draw_text(2, 6, "YT Stream", 1);
  host_draw_text(2, 18, "Auto stream", 1);
  host_draw_text(2, 30, status, 1);
};
