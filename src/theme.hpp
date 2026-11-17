#pragma once

/**
 * @file theme.hpp
 * @brief Colour themes: the terminal background cx assumes, and the
 *        truecolor SGR sequences the renderers use against it.
 *
 * cx cannot reliably know the terminal's colours, so the user picks the
 * theme on the command line (--theme=light|dark). Every palette entry is
 * a complete SGR sequence that sets only a foreground or only a
 * background, so it can be layered over whatever else is active.
 */

#include <string_view>

/** @brief Which terminal background the colours are chosen for. */
enum class Theme {
  Dark,  /**< Dark background, light text (the default). */
  Light, /**< Light background, dark text. */
};

/** @brief The colours one theme renders with. */
struct Palette {
  /** @name Visible whitespace (Esc-w) */
  /** @{ */
  std::string_view wsFg;    /**< Foreground of the ·, ■► and ↵ markers. */
  std::string_view spaceBg; /**< Behind a visible space. */
  std::string_view tabWsBg; /**< Behind a visible tab. */
  std::string_view tabBg;   /**< Behind a tab while whitespace is hidden. */
  std::string_view nlBg;    /**< Behind the end-of-line marker. */
  /** @} */

  /** @name Syntax foregrounds */
  /** @{ */
  std::string_view keyword;
  std::string_view comment;
  std::string_view string;
  std::string_view op;
  std::string_view number;
  std::string_view preproc;
  std::string_view variable; /**< Shell $VAR / ${...}. */
  std::string_view heading;  /**< Markdown heading; also turns on bold. */
  std::string_view link;     /**< Markdown link. */
  std::string_view bracketMatch;
  /** @} */

  /** @name Diff and merge-conflict lines */
  /** @{ */
  std::string_view diffAddBg;
  std::string_view diffRemoveBg;
  std::string_view diffHunkFg;
  std::string_view conflictOursBg;
  std::string_view conflictDivBg;
  std::string_view conflictTheirsBg;
  /** @} */

  /**
   * @brief Background of the Esc-_ current-line highlight in text
   *        buffers. A faint tint rather than reverse video, so the
   *        selection (reverse video), syntax colours and find match stay
   *        legible on it. Distinct from the whitespace greys.
   */
  std::string_view currentLineBg;
  std::string_view findBg; /**< Behind the current find match. */
};

inline constexpr Palette kDarkPalette{
    .wsFg = "\x1b[38;2;140;60;60m",
    .spaceBg = "\x1b[48;2;45;45;45m",
    .tabWsBg = "\x1b[48;2;60;60;60m",
    .tabBg = "\x1b[48;2;70;70;70m",
    .nlBg = "\x1b[48;2;35;35;35m",
    .keyword = "\x1b[38;2;80;130;200m",
    .comment = "\x1b[38;2;80;160;80m",
    .string = "\x1b[38;2;200;80;80m",
    .op = "\x1b[38;2;150;100;200m",
    .number = "\x1b[38;2;100;200;200m",
    .preproc = "\x1b[38;2;180;140;60m",
    .variable = "\x1b[38;2;210;150;90m",
    .heading = "\x1b[1;38;2;80;130;200m",
    .link = "\x1b[38;2;100;180;180m",
    .bracketMatch = "\x1b[38;2;60;170;170m",
    .diffAddBg = "\x1b[48;2;30;60;30m",
    .diffRemoveBg = "\x1b[48;2;70;30;30m",
    .diffHunkFg = "\x1b[38;2;120;160;220m",
    .conflictOursBg = "\x1b[48;2;40;50;80m",
    .conflictDivBg = "\x1b[48;2;70;60;30m",
    .conflictTheirsBg = "\x1b[48;2;50;70;40m",
    .currentLineBg = "\x1b[48;2;38;38;48m",
    .findBg = "\x1b[48;2;60;100;160m",
};

inline constexpr Palette kLightPalette{
    .wsFg = "\x1b[38;2;190;90;90m",
    .spaceBg = "\x1b[48;2;232;232;232m",
    .tabWsBg = "\x1b[48;2;215;215;215m",
    .tabBg = "\x1b[48;2;205;205;205m",
    .nlBg = "\x1b[48;2;242;242;242m",
    .keyword = "\x1b[38;2;0;70;170m",
    .comment = "\x1b[38;2;40;120;40m",
    .string = "\x1b[38;2;170;30;30m",
    .op = "\x1b[38;2;120;50;160m",
    .number = "\x1b[38;2;0;125;125m",
    .preproc = "\x1b[38;2;140;95;0m",
    .variable = "\x1b[38;2;170;90;20m",
    .heading = "\x1b[1;38;2;0;70;170m",
    .link = "\x1b[38;2;0;115;115m",
    .bracketMatch = "\x1b[38;2;0;150;150m",
    .diffAddBg = "\x1b[48;2;210;240;210m",
    .diffRemoveBg = "\x1b[48;2;245;210;210m",
    .diffHunkFg = "\x1b[38;2;40;80;170m",
    .conflictOursBg = "\x1b[48;2;210;220;245m",
    .conflictDivBg = "\x1b[48;2;240;230;190m",
    .conflictTheirsBg = "\x1b[48;2;220;240;205m",
    .currentLineBg = "\x1b[48;2;225;230;245m",
    .findBg = "\x1b[48;2;160;200;250m",
};

/** @brief The palette for @p theme. */
constexpr Palette const& palette(Theme theme) {
  return theme == Theme::Light ? kLightPalette : kDarkPalette;
}
