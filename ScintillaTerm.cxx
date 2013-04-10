// Copyright 2012-2013 Mitchell mitchell.att.foicica.com.
// Scintilla implemented in a UNIX terminal environment.
// Contains platform facilities and a terminal-specific subclass of
// ScintillaBase.
// Note: setlocale(LC_CTYPE, "") must be called before initializing curses in
// order to display UTF-8 characters properly in ncursesw.

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>
#include <map>

#include "Platform.h"

#include "ILexer.h"
#include "Scintilla.h"
#ifdef SCI_LEXER
#include "SciLexer.h"
#endif
#include "SVector.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "CallTip.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "XPM.h"
#include "LineMarker.h"
#include "Style.h"
#include "AutoComplete.h"
#include "ViewStyle.h"
#include "Decoration.h"
#include "CharClassify.h"
#include "Document.h"
#include "Selection.h"
#include "PositionCache.h"
#include "Editor.h"
#include "ScintillaBase.h"
#include "UniConversion.h"
#include "ScintillaTerm.h"

#if SCI_LEXER
#include "LexerModule.h"
#include "ExternalLexer.h"
#endif

/**
 * Returns the given Scintilla `WindowID` as a curses `WINDOW`.
 * @param w A Scintilla `WindowID`.
 * @return curses `WINDOW`.
 */
#define _WINDOW(w) reinterpret_cast<WINDOW *>(w)

// Defines for getting attributes for INDIC_ROUNDBOX and INDIC_STRAIGHTBOX.
// These are specific to curses implementations.
#if NCURSES_VERSION_MAJOR
#ifdef NCURSES_WIDECHAR
#define wattrget(w, y, x) (w)->_line[(y)].text[(x)].attr;
#undef NCURSES_CH_T
#define NCURSES_CH_T cchar_t
#else
#define wattrget(w, y, x) (w)->_line[(y)].text[(x)];
#endif
struct ldat {
  NCURSES_CH_T *text;
  NCURSES_SIZE_T firstchar;
  NCURSES_SIZE_T lastchar;
  NCURSES_SIZE_T oldindex;
};
#elif PDCURSES
#define wattrget(w, y, x) (w)->_y[(y)][(x)];
#else
#define wattrget(w, y, x) 0
#endif

// Font handling.

/**
 * Allocates a new Scintilla font for the terminal.
 * Since the terminal handles fonts on its own, the only use for Scintilla font
 * objects is to indicate which attributes terminal characters have. This is
 * done in `Font::Create()`.
 * @see Font::Create
 */
Font::Font() : fid(0) {}
/** Deletes the font. Currently empty. */
Font::~Font() {}
/**
 * Sets terminal character attributes for a particular font.
 * These attributes are a union of curses attributes and stored in the font's
 * `fid`.
 * The curses attributes are not constructed from various fields in *fp* since
 * there is no `underline` parameter. Instead, you need to manually set the
 * `weight` parameter to be the union of your desired attributes.
 * Scintillua (http://foicica.com/scintillua) has an example of this.
 * @param fp Scintilla font parameters.
 */
void Font::Create(const FontParameters &fp) {
  Release();
  attr_t attrs = 0;
  if (fp.weight == SC_WEIGHT_BOLD)
    attrs = A_BOLD;
  else if (fp.weight != SC_WEIGHT_NORMAL && fp.weight != SC_WEIGHT_SEMIBOLD)
    attrs = fp.weight; // font attributes are stored in fp.weight
  fid = reinterpret_cast<FontID>(attrs);
}
/** Releases a font's resources. */
void Font::Release() { fid = 0; }

// Color handling.

static int COLOR_LBLACK = COLOR_BLACK + 8;
static int COLOR_LRED = COLOR_RED + 8;
static int COLOR_LGREEN = COLOR_GREEN + 8;
static int COLOR_LYELLOW = COLOR_YELLOW + 8;
static int COLOR_LBLUE = COLOR_BLUE + 8;
static int COLOR_LMAGENTA = COLOR_MAGENTA + 8;
static int COLOR_LCYAN = COLOR_CYAN + 8;
static int COLOR_LWHITE = COLOR_WHITE + 8;

static bool inited_colors = false;

/**
 * Initializes colors in curses if they have not already been initialized.
 * Creates all possible color pairs using the `SCI_COLOR_PAIR()` macro.
 * This is called automatically from `scintilla_new()`.
 */
void init_colors() {
  if (inited_colors) return;
  if (has_colors()) {
    start_color();
    for (int back = 0; back < ((COLORS < 16) ? 8 : 16); back++)
      for (int fore = 0; fore < ((COLORS < 16) ? 8 : 16); fore++)
        init_pair(SCI_COLOR_PAIR(fore, back), fore, back);
    if (COLORS < 16) {
      COLOR_LBLACK -= 8;
      COLOR_LRED -= 8;
      COLOR_LGREEN -= 8;
      COLOR_LYELLOW -= 8;
      COLOR_LBLUE -= 8;
      COLOR_LMAGENTA -= 8;
      COLOR_LCYAN -= 8;
      COLOR_LWHITE -= 8;
    }
  }
  inited_colors = true;
}

static ColourDesired BLACK(0, 0, 0);
static ColourDesired RED(0x80, 0, 0);
static ColourDesired GREEN(0, 0x80, 0);
static ColourDesired YELLOW(0x80, 0x80, 0);
static ColourDesired BLUE(0, 0, 0x80);
static ColourDesired MAGENTA(0x80, 0, 0x80);
static ColourDesired CYAN(0, 0x80, 0x80);
static ColourDesired WHITE(0xC0, 0xC0, 0xC0);
static ColourDesired LBLACK(0x40, 0x40, 0x40);
static ColourDesired LRED(0xFF, 0, 0);
static ColourDesired LGREEN(0, 0xFF, 0);
static ColourDesired LYELLOW(0xFF, 0xFF, 0);
static ColourDesired LBLUE(0, 0, 0xFF);
static ColourDesired LMAGENTA(0xFF, 0, 0xFF);
static ColourDesired LCYAN(0, 0xFF, 0xFF);
static ColourDesired LWHITE(0xFF, 0xFF, 0xFF);

/**
 * Returns a curses color for the given Scintilla color.
 * Recognized colors are: black (0x000000), red (0x800000), green (0x008000),
 * yellow (0x808000), blue (0x000080), magenta (0x800080), cyan (0x008080),
 * white (0xc0c0c0), light black (0x404040), light red (0xff0000), light green
 * (0x00ff00), light yellow (0xffff00), light blue (0x0000ff), light magenta
 * (0xff00ff), light cyan (0x00ffff), and light white (0xffffff). If the color
 * is not recognized, returns `COLOR_WHITE` by default.
 * @param color Color to get a curses color for.
 * @return curses color
 */
static int term_color(ColourDesired color) {
  if (color == BLACK) return COLOR_BLACK;
  else if (color == RED) return COLOR_RED;
  else if (color == GREEN) return COLOR_GREEN;
  else if (color == YELLOW) return COLOR_YELLOW;
  else if (color == BLUE) return COLOR_BLUE;
  else if (color == MAGENTA) return COLOR_MAGENTA;
  else if (color == CYAN) return COLOR_CYAN;
  else if (color == LBLACK) return COLOR_LBLACK;
  else if (color == LRED) return COLOR_LRED;
  else if (color == LGREEN) return COLOR_LGREEN;
  else if (color == LYELLOW) return COLOR_LYELLOW;
  else if (color == LBLUE) return COLOR_LBLUE;
  else if (color == LMAGENTA) return COLOR_LMAGENTA;
  else if (color == LCYAN) return COLOR_LCYAN;
  else if (color == LWHITE) return COLOR_LWHITE;
  else return COLOR_WHITE;
}

/**
 * Returns a curses color for the given curses color.
 * This overloaded method only exists for the `term_color_pair()` macro.
 */
static int term_color(int color) { return color; }

/**
 * Returns a curses color pair from the given fore and back colors.
 * @param f Foreground color, either a Scintilla color or curses color.
 * @param b Background color, either a Scintilla color or curses color.
 * @return curses color pair suitable for calling `COLOR_PAIR()` with.
 */
#define term_color_pair(f, b) SCI_COLOR_PAIR(term_color(f), term_color(b))

// Surface handling.

/**
 * Implementation of a Scintilla surface for the terminal.
 * The surface is initialized with a curses `WINDOW` for drawing on. Since the
 * terminal can only show text, many of Scintilla's pixel-based functions are
 * not implemented.
 */
class SurfaceImpl : public Surface {
  WINDOW *win;
public:
  /** Allocates a new Scintilla surface for the terminal. */
  SurfaceImpl() : win(0) {}
  /** Deletes the surface. */
  ~SurfaceImpl() { Release(); }

  /**
   * Initializes/reinitializes the surface with a curses `WINDOW` for drawing
   * on.
   * @param wid Curses `WINDOW`.
   */
  void Init(WindowID wid) {
    Release();
    win = _WINDOW(wid);
  }
  /**
   * Initializes the surface with an existing surface for drawing on.
   * @param sid Existing surface.
   * @param wid Curses `WINDOW`. Not used.
   */
  void Init(SurfaceID sid, WindowID wid) { Init(sid); }
  /** Initializing the surface as a pixmap is not implemented. */
  void InitPixMap(int width, int height, Surface *surface_, WindowID wid) {}

  /** Releases the surface's resources. */
  void Release() { win = 0; }
  /**
   * Returns `true` since this method is only called for pixmap surfaces and
   * those surfaces are not implemented.
   */
  bool Initialised() { return true; }
  /**
   * Setting the surface's foreground color is not implemented because all uses
   * in Scintilla involve special drawing that is not supported in curses.
   */
  void PenColour(ColourDesired fore) {}
  /** Unused; return value irrelevant. */
  int LogPixelsY() { return 1; }
  /** Returns 1 since font height is always 1 in the terminal. */
  int DeviceHeightFont(int points) { return 1; }
  void MoveTo(int x_, int y_) {}
  void LineTo(int x_, int y_) {}
  /** Drawing polygons is not implemented. */
  void Polygon(Point *pts, int npts, ColourDesired fore, ColourDesired back) {}
  /** Drawing rectangles in Scintilla's sense is not implemented. */
  void RectangleDraw(PRectangle rc, ColourDesired fore, ColourDesired back) {}
  /**
   * Clears the given portion of the screen with the given background color.
   * In some cases, it can be determined that whitespace is being drawn. If so,
   * draw it appropriately instead of clearing the given portion of the screen.
   * @param rc The portion of the screen to clear.
   * @param back The background color to use.
   */
  void FillRectangle(PRectangle rc, ColourDesired back) {
    wattr_set(win, 0, term_color_pair(COLOR_WHITE, back), NULL);
    chtype ch = ' ';
    if (fabs(rc.left - (int)rc.left) > 0.1) {
      // If rc.left is a fractional value (e.g. 4.5) then whitespace dots are
      // being drawn. Draw them appropriately.
      wcolor_set(win, term_color_pair(COLOR_BLACK, COLOR_BLACK), NULL);
      rc.right = (int)rc.right, ch = ACS_BULLET | A_BOLD;
    }
    for (int y = rc.top; y < rc.bottom; y++)
      for (int x = rc.left; x < rc.right; x++)
        mvwaddch(win, y, x, ch);
  }
  /**
   * Instead of filling a portion of the screen with a surface pixmap, fills the
   * the screen portion with black.
   * @param rc The portion of the screen to fill.
   * @param surfacePattern Unused.
   */
  void FillRectangle(PRectangle rc, Surface &surfacePattern) {
    FillRectangle(rc, BLACK);
  }
  /** Drawing rounded rectangles is not implemented. */
  void RoundedRectangle(PRectangle rc, ColourDesired fore,
                        ColourDesired back) {}
  /**
   * Drawing alpha rectangles is not fully supported.
   * Instead, fill the background color with the fill color, emulating
   * INDIC_STRAIGHTBOX with no transparency.
   */
  void AlphaRectangle(PRectangle rc, int cornerSize, ColourDesired fill,
                      int alphaFill, ColourDesired outline, int alphaOutline,
                      int flags) {
    int x = rc.left, y = rc.top - 1, len = rc.right - rc.left;
    attr_t attrs = wattrget(win, y, x);
    short pair = PAIR_NUMBER(attrs), fore = COLOR_WHITE;
    if (pair > 0) pair_content(pair, &fore, NULL);
    mvwchgat(win, y, x, len, attrs, term_color_pair(fore, fill), NULL);
  }
  /** Drawing images is not implemented. */
  void DrawRGBAImage(PRectangle rc, int width, int height,
                     const unsigned char *pixelsImage) {}
  /** Drawing ellipses is not implemented. */
  void Ellipse(PRectangle rc, ColourDesired fore, ColourDesired back) {}
  /** Copying surfaces is not implemented. */
  void Copy(PRectangle rc, Point from, Surface &surfaceSource) {}

  /**
   * Draw the given text at the given position on the screen with the given
   * foreground and background colors.
   * @param rc The point on the screen to draw the given text at.
   * @param font_ The current font for setting curses character attributes.
   * @param ybase Unused.
   * @param s The text to draw.
   * @param len The length of the text to draw.
   * @param fore The Scintilla foreground color to draw the text in.
   * @param back The Scintilla background color to draw the text in.
   */
  void DrawTextNoClip(PRectangle rc, Font &font_, XYPOSITION ybase,
                      const char *s, int len, ColourDesired fore,
                      ColourDesired back) {
    intptr_t attrs = reinterpret_cast<intptr_t>(font_.GetID());
    wattr_set(win, static_cast<attr_t>(attrs), term_color_pair(fore, back),
              NULL);
    if (rc.left < 0) s += static_cast<int>(-rc.left), rc.left = 0;
    mvwaddnstr(win, rc.top, rc.left, s, Platform::Minimum(len, COLS - rc.left));
  }
  /**
   * Similar to `DrawTextNoClip()`.
   * Called for drawing the caret, control characters, and line markers.
   * When drawing control characters, `rc` needs to have its pixel padding
   * removed since curses has smaller resolution. Similarly when drawing line
   * markers, `rc` needs to be reshaped.
   * @see DrawTextNoClip
   */
  void DrawTextClipped(PRectangle rc, Font &font_, XYPOSITION ybase,
                       const char *s, int len, ColourDesired fore,
                       ColourDesired back) {
    if (rc.left >= rc.right) // when drawing control characters
      rc.left -= 2, rc.right -= 2, rc.top -= 1, rc.bottom -= 1;
    else if (rc.top > rc.bottom)
      rc.top -= 1, rc.bottom += 1;
    DrawTextNoClip(rc, font_, ybase, s, len, fore, back);
  }
  /**
   * Similar to `DrawTextNoClip()`.
   * Called for drawing CallTip text.
   * This would also be called for two-phase drawing, but that is not supported.
   */
  void DrawTextTransparent(PRectangle rc, Font &font_, XYPOSITION ybase,
                           const char *s, int len, ColourDesired fore) {
    DrawTextNoClip(rc, font_, ybase, s, len, fore, BLACK);
  }
  /**
   * Measures the width of characters in the given string.
   * Terminal font characters always have a width of 1 if they are not UTF-8
   * trailing bytes.
   * @param font_ Unused.
   * @param s The string to measure.
   * @param len The length of the string.
   * @param positions The string's character widths.
   */
  void MeasureWidths(Font &font_, const char *s, int len,
                     XYPOSITION *positions) {
    for (int i = 0, j = 0; i < len; i++) {
      if (!UTF8IsTrailByte((unsigned char)s[i])) j++;
      positions[i] = j;
    }
  }
  /**
   * Returns the length of the string since terminal font characters always have
   * a width of 1.
   */
  XYPOSITION WidthText(Font &font_, const char *s, int len) { return len; }
  /** Returns 1 since terminal font characters always have a width of 1. */
  XYPOSITION WidthChar(Font &font_, char ch) { return 1; }
  /** Returns 0 since terminal font characters have no ascent. */
  XYPOSITION Ascent(Font &font_) { return 0; }
  /** Returns 0 since terminal font characters have no descent. */
  XYPOSITION Descent(Font &font_) { return 0; }
  /** Returns 0 since terminal font characters have no leading. */
  XYPOSITION InternalLeading(Font &font_) { return 0; }
  /** Returns 0 since terminal font characters have no leading. */
  XYPOSITION ExternalLeading(Font &font_) { return 0; }
  /** Returns 1 since terminal font characters always have a height of 1. */
  XYPOSITION Height(Font &font_) { return 1; }
  /** Returns 1 since terminal font characters always have a width of 1. */
  XYPOSITION AverageCharWidth(Font &font_) { return 1; }

  /** Setting clips is not implemented. */
  void SetClip(PRectangle rc) {}
  /** Flushing cache is not implemented. */
  void FlushCachedState() {}

  void SetUnicodeMode(bool unicodeMode_) {}
  void SetDBCSMode(int codePage) {}
};

/** Creates a new terminal surface. */
Surface *Surface::Allocate(int) { return new SurfaceImpl(); }

// Window handling.

/** Deletes the window. */
Window::~Window() {}
/**
 * Releases the window's resources.
 * Since the only Windows created are AutoComplete and CallTip windows, and
 * those windows are created in `ListBox::Create()` and
 * `ScintillaTerm::CreateCallTipWindow()` respectively via `newwin()`, it is
 * safe to use `delwin()`.
 * It is important to note that even though `ScintillaTerm::wMain` is a Window,
 * its `Destroy()` function is never called, hence why `scintilla_delete()` is
 * the complement to `scintilla_new()`.
 */
void Window::Destroy() {
  if (wid) delwin(_WINDOW(wid));
  wid = 0;
}
/**
 * Returns the window's boundaries
 * Unlike other platforms, Scintilla paints in coordinates relative to the
 * window in curses. Therefore, this function should always return the window
 * bounds to ensure all of it is painted.
 * @return PRectangle with the window's boundaries.
 */
PRectangle Window::GetPosition() {
  return PRectangle(0, 0, getmaxx(_WINDOW(wid)), getmaxy(_WINDOW(wid)));
}
/**
 * Sets the position of the window relative to its parent window.
 * It will take care not to exceed the boundaries of the parent.
 * @param rc The position relative to the parent window.
 * @param relativeTo the parent window.
 */
void Window::SetPositionRelative(PRectangle rc, Window relativeTo) {
  int begx = 0, begy = 0, x = 0, y = 0;
  // Determine the relative position.
  getbegyx(_WINDOW(relativeTo.GetID()), begy, begx);
  x = begx + rc.left;
  if (x < begx) x = begx;
  y = begy + rc.top;
  if (y < begy) y = begy;
  // Correct to fit the parent if necessary.
  int sizex = rc.right - rc.left;
  if (x > 0) sizex -= 1; // in curses, x pos counts as "1" width
  int sizey = rc.bottom - rc.top;
  if (y > 0) sizey -= 1; // in curses, y pos counts as "1" height
  int screen_width = getmaxx(_WINDOW(relativeTo.GetID()));
  int screen_height = getmaxy(_WINDOW(relativeTo.GetID()));
  if (sizex > screen_width)
    x = begx;
  else if (x + sizex > screen_width)
    x = screen_width - sizex;
  if (sizey > screen_height)
    y = begy;
  else if (y + sizey > screen_height)
    y = screen_height - sizey;
  // Update the location.
  mvwin(_WINDOW(wid), y, x);
}
/** Identical to `Window::GetPosition()`. */
PRectangle Window::GetClientPosition() { return GetPosition(); }
void Window::Show(bool show) { /* TODO: */ }
void Window::InvalidateAll() { /* notify repaint */ }
void Window::InvalidateRectangle(PRectangle rc) { /* notify repaint*/ }
/** Setting the font is not implemented. */
void Window::SetFont(Font &) {}
/** Setting the cursor icon is not implemented. */
void Window::SetCursor(Cursor curs) {}
/** Identical to `Window::GetPosition()`. */
PRectangle Window::GetMonitorRect(Point pt) { return GetPosition(); }

/**
 * Implementation of a Scintilla ListBox for the terminal.
 * Instead of registering images to types, printable characters are registered
 * to types.
 */
class ListBoxImpl : public ListBox {
  int height, width;
  std::vector<std::string> list;
  char types[10]; // 0-9
  int selection;
public:
  /** Allocates a new Scintilla ListBox for the terminal. */
  ListBoxImpl() : height(5), width(10), selection(0) {
    list.reserve(10);
    ClearRegisteredImages();
  }
  /** Deletes the ListBox. */
  virtual ~ListBoxImpl() {}

  /** Setting the font is not implemented. */
  virtual void SetFont(Font &font) {}
  /**
   * Creates a new listbox.
   * The `Show()` function resizes window with the appropriate height and width.
   * @param parent Unused.
   * @param ctrlID Unused.
   * @param location_ Unused.
   * @param lineHeight_ Unused.
   * @param unicodeMode_ Unused.
   * @param technology_ Unused.
   */
  virtual void Create(Window &parent, int ctrlID, Point location_,
                      int lineHeight_, bool unicodeMode_, int technology_) {
    wid = newwin(1, 1, 0, 0);
  }
  /**
   * Setting average char width is not implemented since all terminal characters
   * have a width of 1.
   */
  virtual void SetAverageCharWidth(int width) {}
  /**
   * Sets the number of visible rows in the listbox.
   * @param rows The number of rows.
   */
  virtual void SetVisibleRows(int rows) {
    height = rows;
    wresize(_WINDOW(wid), height + 2, width + 2);
  }
  /**
   * Gets the number of visible rows in the listbox.
   * @return int number of rows.
   */
  virtual int GetVisibleRows() const { return height; }
  /**
   * Gets the desired size of the listbox.
   * @return desired size.
   */
  virtual PRectangle GetDesiredRect() {
    return PRectangle(0, 0, width + 2, height + 2); // add border widths
  }
  /**
   * Returns the left-offset of the ListBox with respect to the caret.
   * Takes into account the border width and type character width.
   * @return 2 to shift the ListBox to the left two characters.
   */
  virtual int CaretFromEdge() { return 2; }
  /** Clears the contents of the listbox. */
  virtual void Clear() {
    list.clear();
    width = 0;
  }
  /**
   * Adds the given string list item to the listbox.
   * Prepends the type character to the list item for display.
   * @param s The string list item to add.
   * @param type The type of the list item (if any).
   */
  virtual void Append(char *s, int type = -1) {
    char chtype = (type >= 0 && type <= 9) ? types[type] : ' ';
    list.push_back(std::string(&chtype, 1) + std::string(s));
    int len = strlen(s);
    if (width < len) {
      width = len + 1; // include type character len
      wresize(_WINDOW(wid), height + 2, width + 2);
    }
  }
  /**
   * Returns the number of items in the listbox.
   * @return int number of items.
   */
  virtual int Length() { return list.size(); }
  /**
   * Selects the given item in the listbox.
   * The listbox is also repainted.
   * @param n The index of the item to select.
   */
  virtual void Select(int n) {
    WINDOW *w = _WINDOW(wid);
    wclear(w);
    box(w, '|', '-');
    int len = static_cast<int>(list.size());
    int s = n - height / 2;
    if (s + height > len) s = len - height;
    if (s < 0) s = 0;
    for (int i = s; i < s + height && i < len; i++) {
      mvwaddstr(w, i - s + 1, 1, list.at(i).c_str());
      if (i == n) mvwchgat(w, i - s + 1, 2, width - 1, A_REVERSE, 0, NULL);
    }
    wmove(w, n - s + 1, 1); // place cursor on selected line
    wrefresh(w);
    selection = n;
  }
  /**
   * Gets the currently selected item in the listbox.
   * @return int index of the selected item.
   */
  virtual int GetSelection() { return selection; }
  /**
   * Searches the listbox for the items matching the given prefix string and
   * returns the index of the first match.
   * Since the type is displayed as the first character, the value starts on the
   * second character; match strings starting there.
   * @param prefix The string prefix to search for.
   * @return int index of the first matching item in the listbox.
   */
  virtual int Find(const char *prefix) {
    int len = strlen(prefix);
    for (unsigned int i = 0; i < list.size(); i++)
      if (strncmp(prefix, list.at(i).c_str() + 1, len) == 0)
        return i;
    return -1;
  }
  /**
   * Gets the item in the listbox at the given index and stores it in the given
   * string.
   * Since the type is displayed as the first character, the value starts on the
   * second character.
   * @param n The index of the listbox item to get.
   * @param value The string to store the listbox item in.
   * @param len The length of `value`.
   */
  virtual void GetValue(int n, char *value, int len) {
    if (len > 0) {
      strncpy(value, list.at(n).c_str() + 1, len);
      value[len - 1] = '\0';
    } else value[0] = '\0';
  }
  /**
   * Registers the first character of the given string to the given type.
   * By default, ' ' (space) is registered to all types.
   * @param type The type to register.
   * @param xpm_data A string whose first character is displayed as the given
   *   type.
   * @usage SCI_REGISTERIMAGE(1, "*") // type 1 shows '*' in front of list item.
   * @usage SCI_REGISTERIMAGE(2, "+") // type 2 shows '+' in front of list item.
   */
  virtual void RegisterImage(int type, const char *xpm_data) {
    if (type >= 0 && type <= 9) types[type] = xpm_data[0];
  }
  /** Registering images is not implemented. */
  virtual void RegisterRGBAImage(int type, int width, int height,
                                 const unsigned char *pixelsImage) {}
  /** Clears all registered types back to ' ' (space). */
  virtual void ClearRegisteredImages() {
    for (int i = 0; i < 10; i++) types[i] = ' ';
  }
  /** Double-clicking is not implemented. */
  virtual void SetDoubleClickAction(CallBackAction action, void *data) {}
  /**
   * Sets the list items in the listbox.
   * @param listText The list of items in string format.
   * @param separator The character separating one item in the string from
   *   another.
   * @param typesep The character separating a list item from a list type.
   */
  virtual void SetList(const char *listText, char separator, char typesep) {
    Clear();
    int len = strlen(listText);
    char *text = new char[len + 1];
    if (text) {
      memcpy(text, listText, len + 1);
      char *word = text, *type = NULL;
      for (int i = 0; i <= len; i++) {
        if (text[i] == separator || i == len) {
          text[i] = '\0';
          if (type) *type = '\0';
          Append(word, type ? atoi(type + 1) : -1);
          word = text + i + 1;
          type = NULL;
        } else if (text[i] == typesep) {
          type = text + i;
        }
      }
      delete []text;
    }
  }
};

/** Creates a new Scintilla ListBox. */
ListBox::ListBox() {}
/** Deletes the ListBox. */
ListBox::~ListBox() {}
/** Creates a new Terminal ListBox. */
ListBox *ListBox::Allocate() { return new ListBoxImpl(); }

Menu::Menu() : mid(0) {}
void Menu::CreatePopUp() {}
void Menu::Destroy() {}
void Menu::Show(Point pt, Window &w) {}

ElapsedTime::ElapsedTime() {}

DynamicLibrary *DynamicLibrary::Load(const char *modulePath) {
  /* TODO */ return 0;
}

ColourDesired Platform::Chrome() { return ColourDesired(0, 0, 0); }
ColourDesired Platform::ChromeHighlight() { return ColourDesired(0, 0, 0); }
const char *Platform::DefaultFont() { return "monospace"; }
int Platform::DefaultFontSize() { return 10; }
unsigned int Platform::DoubleClickTime() { return 500; /* ms */ }
bool Platform::MouseButtonBounce() { return true; }
void Platform::DebugDisplay(const char *s) { fprintf(stderr, "%s", s); }
//bool Platform::IsKeyDown(int key) { return false; }
//long Platform::SendScintilla(WindowID w, unsigned int msg,
//                             unsigned long wParam, long lParam) { return 0; }
//long Platform::SendScintillaPointer(WindowID w, unsigned int msg,
//                                    unsigned long wParam,
//                                    void *lParam) { return 0; }
//bool Platform::IsDBCSLeadByte(int codePage, char ch) { return false; }
//int Platform::DBCSCharLength(int codePage, const char *s) {
//  int bytes = mblen(s, MB_CUR_MAX);
//  return (bytes >= 1) ? bytes : 1;
//}
//int Platform::DBCSCharMaxLength() { return MB_CUR_MAX; }
int Platform::Minimum(int a, int b) { return (a < b) ? a : b; }
int Platform::Maximum(int a, int b) { return (a > b) ? a : b; }
void Platform::DebugPrintf(const char *format, ...) {}
//bool Platform::ShowAssertionPopUps(bool assertionPopUps_) { return true; }
void Platform::Assert(const char *c, const char *file, int line) {
  char buffer[2000];
  sprintf(buffer, "Assertion [%s] failed at %s %d\r\n", c, file, line);
  Platform::DebugDisplay(buffer);
  abort();
}
int Platform::Clamp(int val, int minVal, int maxVal) {
  if (val > maxVal) val = maxVal;
  if (val < minVal) val = minVal;
  return val;
}

/** Implementation of Scintilla for the Terminal. */
class ScintillaTerm : public ScintillaBase {
  Surface *sur;
  void (*callback)(Scintilla *, int, void *, void *);
  SelectionText clipboard;
public:
  /**
   * Creates a new Scintilla instance in a curses `WINDOW`.
   * The `WINDOW` is initially full-screen.
   * @param callback_ Callback function for Scintilla notifications.
   */
  ScintillaTerm(void (*callback_)(Scintilla *, int, void *, void *)) {
    wMain = newwin(0, 0, 0, 0);
    keypad(GetWINDOW(), TRUE);
    callback = callback_;
    if ((sur = Surface::Allocate(SC_TECHNOLOGY_DEFAULT)))
      sur->Init(GetWINDOW());

    // Defaults for terminals.
    bufferedDraw = false; // draw directly to the screen
    twoPhaseDraw = false; // no need for this
    horizontalScrollBarVisible = false; // no scroll bars
    verticalScrollBarVisible = false; // no scroll bars
    vs.selforeset = true; // setting selection foreground below
    vs.selforeground = ColourDesired(0, 0, 0); // black on white selection
    vs.caretcolour = ColourDesired(0xFF, 0xFF, 0xFF); // white caret
    vs.caretStyle = CARETSTYLE_BLOCK; // block caret
    vs.leftMarginWidth = 0; // no margins
    vs.rightMarginWidth = 0; // no margins
    vs.ms[1].style = SC_MARGIN_TEXT; // markers are text-based, not pixmap-based
    vs.ms[1].width = 1; // marker margin width should be 1
    vs.ms[2].style = SC_MARGIN_TEXT; // markers are text-based, not pixmap-based
    vs.extraDescent = -1; // hack to make lineHeight 1 instead of 2
    // Use '+' and '-' fold markers.
    vs.markers[SC_MARKNUM_FOLDEROPEN].markType = SC_MARK_CHARACTER + '-';
    vs.markers[SC_MARKNUM_FOLDEROPEN].fore = ColourDesired(0xFF, 0xFF, 0xFF);
    vs.markers[SC_MARKNUM_FOLDEROPEN].back = ColourDesired(0, 0, 0);
    vs.markers[SC_MARKNUM_FOLDER].markType = SC_MARK_CHARACTER + '+';
    vs.markers[SC_MARKNUM_FOLDER].fore = ColourDesired(0xFF, 0xFF, 0xFF);
    vs.markers[SC_MARKNUM_FOLDER].back = ColourDesired(0, 0, 0);
    displayPopupMenu = false; // no context menu
    marginNumberPadding = 0; // no number margin padding
    ctrlCharPadding = 0; // no ctrl character text blob padding
    lastSegItalicsOffset = 0; // no offset for italic characters at EOLs
    ac.widthLBDefault = 10; // more sane bound for autocomplete width
    ac.heightLBDefault = 10; // more sane bound for autocomplete  height
    ct.colourSel = ColourDesired(0, 0, 0xFF);
    ct.insetX = 2; // border and arrow widths are 1 each
    ct.widthArrow = 1; // arrow width is 1 character
    ct.borderHeight = 1; // no extra empty lines in border height
    ct.verticalOffset = 0; // no extra offset of calltip from line
  }
  /**
   * Deletes the Scintilla instance.
   */
  virtual ~ScintillaTerm() {
    delwin(GetWINDOW());
    if (sur) {
      sur->Release();
      delete sur;
    }
  }
  /**
   * Sends the given message and parameters to Scintilla.
   * @param iMessage The message ID.
   * @param wParam The first parameter.
   * @param lParam The second parameter.
   */
  virtual sptr_t WndProc(unsigned int iMessage, uptr_t wParam, uptr_t lParam) {
    switch (iMessage) {
      case SCI_GETDIRECTFUNCTION:
        return reinterpret_cast<sptr_t>(scintilla_send_message);
      case SCI_GETDIRECTPOINTER: return reinterpret_cast<sptr_t>(this);
      default: return ScintillaBase::WndProc(iMessage, wParam, lParam);
    }
    return 0;
  }
  /** Extra initialising code is unnecessary. */
  virtual void Initialise() {}
  /** Extra finalising code is unnecessary. */
  virtual void Finalise() {}
  /** Setting scroll positions is not implemented. */
  virtual void SetVerticalScrollPos() {}
  /** Setting scroll positions is not implemented. */
  virtual void SetHorizontalScrollPos() {}
  /** Modifying scrollbars is not implemented. */
  virtual bool ModifyScrollBars(int nMax, int nPage) { return false; }
  /**
   * Copies the selected text to the internal clipboard.
   * The primary and secondary X selections are unaffected.
   */
  virtual void Copy() {
    if (!sel.Empty()) CopySelectionRange(&clipboard);
  }
  /**
   * Pastes text from the internal clipboard, not from primary or secondary X
   * selections.
   */
  virtual void Paste() {
    if (!clipboard.s) return;
    ClearSelection(multiPasteMode == SC_MULTIPASTE_EACH);
    SelectionPosition sp = !sel.IsRectangular() ? sel.Range(sel.Main()).Start()
                                                : sel.Rectangular().Start();
    if (!clipboard.rectangular) {
      int len = clipboard.len - 1;
      const char *s = Document::TransformLineEnds(&len, clipboard.s, len,
                                                  pdoc->eolMode);
      InsertPaste(sp, s, len);
      delete []s;
    } else PasteRectangular(sp, clipboard.s, clipboard.len - 1);
    EnsureCaretVisible();
  }
  /** Setting of the primary and/or secondary X selections is not supported. */
  virtual void ClaimSelection() {}
  /** Notifying the parent of text changes is not yet supported. */
  virtual void NotifyChange() {}
  /** Send Scintilla notifications to the parent. */
  virtual void NotifyParent(SCNotification scn) {
    if (callback)
      (*callback)(reinterpret_cast<Scintilla *>(this), 0, (void *)&scn, 0);
  }
  /**
   * Handles an unconsumed key.
   * If a character is being typed, add it to the editor. Otherwise, notify the
   * container.
   * @param key The keycode of the key.
   * @param modifiers A bitmask of modifiers for `key`.
   */
  virtual int KeyDefault(int key, int modifiers) {
    if (key < 256 && !(modifiers & (SCMOD_CTRL | SCMOD_ALT | SCMOD_META))) {
      AddChar(key);
      return 1;
    } else {
      SCNotification scn = {0};
      scn.nmhdr.code = SCN_KEY;
      scn.ch = key;
      scn.modifiers = modifiers;
      NotifyParent(scn);
      return 0;
    }
  }
  /**
   * Copies the given text to the internal clipboard.
   * Like `Copy()`, does not affect the primary and secondary X selections.
   * @param selectedText The text to copy.
   */
  virtual void CopyToClipboard(const SelectionText &selectedText) {
    clipboard.Copy(selectedText);
  }
  /** A ticking caret is not implemented. */
  virtual void SetTicking(bool on) {}
  /** Mouse capture is not implemented. */
  virtual void SetMouseCapture(bool on) {}
  /** Mouse capture is not implemented. */
  virtual bool HaveMouseCapture() { return false; }
  /** A Scintilla direct pointer is not implemented. */
  virtual sptr_t DefWndProc(unsigned int iMessage, uptr_t wParam,
                            sptr_t lParam) { return 0; }
  /**
   * Draws a CallTip, creating the curses window for it if necessary.
   * @param rc The bounds of the CallTip window. Ignored when redrawing the
   * CallTip.
   */
  virtual void CreateCallTipWindow(PRectangle rc) {
    if (!ct.wCallTip.Created()) {
      rc.right -= 1; // remove right-side padding
      int begx = 0, begy = 0, maxx = 0, maxy = 0;
      getbegyx(GetWINDOW(), begy, begx);
      int xoffset = begx - rc.left, yoffset = begy - rc.top;
      if (xoffset > 0) rc.left += xoffset, rc.right += xoffset;
      if (yoffset > 0) rc.top += yoffset, rc.bottom += yoffset;
      getmaxyx(GetWINDOW(), maxy, maxx);
      if (rc.Width() > maxx) rc.right = rc.left + maxx;
      if (rc.Height() > maxy) rc.bottom = rc.top + maxy;
      ct.wCallTip = newwin(rc.Height(), rc.Width(), rc.top, rc.left);
    }
    WindowID wid = ct.wCallTip.GetID();
    box(_WINDOW(wid), '|', '-');
    Surface *sur = Surface::Allocate(SC_TECHNOLOGY_DEFAULT);
    if (sur) {
      sur->Init(wid);
      ct.PaintCT(sur);
      wrefresh(_WINDOW(wid));
      sur->Release();
      delete sur;
    }
  }
  virtual void AddToPopUp(const char *label, int cmd=0, bool enabled=true) {}

  /**
   * Gets the curses `WINDOW` associated with this Scintilla instance.
   * @return curses `WINDOW`.
   */
  WINDOW *GetWINDOW() { return _WINDOW(wMain.GetID()); }
  /** Repaints the Scintilla window. */
  void Refresh() {
    WINDOW *w = GetWINDOW();
    rcPaint.top = 0, rcPaint.left = 0; // paint from (0, 0), not (begy, begx)
    getmaxyx(w, rcPaint.bottom, rcPaint.right);
    Paint(sur, rcPaint);
    wrefresh(w);
    if (ac.Active())
      ac.lb->Select(ac.lb->GetSelection()); // redraw
    else if (ct.inCallTipMode)
      CreateCallTipWindow(PRectangle(0, 0, 0, 0)); // redraw
  }
  /**
   * Sends a key to Scintilla.
   * Usually if a key is consumed, the screen should be repainted. However, when
   * autocomplete is active, that window is consuming the keys and any
   * repainting of the main Scintilla window will overwrite the autocomplete
   * window.
   * @param key The key pressed.
   * @param shift Flag indicating whether or not the shift modifier key is
   *   pressed.
   * @param shift Flag indicating whether or not the control modifier key is
   *   pressed.
   * @param shift Flag indicating whether or not the alt modifier key is
   *   pressed.
   */
  void KeyPress(int key, bool shift, bool ctrl, bool alt) {
    bool consumed = false;
    KeyDown(key, shift, ctrl, alt, &consumed);
  }
  /**
   * Copies the text of the internal clipboard, not the primary and/or secondary
   * X selections, into the given buffer and returns the size of the clipboard
   * text.
   * @param text The buffer to copy clipboard text to.
   * @return size of the clipboard text
   */
  int GetClipboard(char *buffer) {
    if (buffer) memcpy(buffer, clipboard.s, clipboard.len);
    return clipboard.len;
  }
};

// Link with C.
extern "C" {
/**
 * Creates a new Scintilla window.
 * @param callback A callback function for Scintilla notifications.
 */
Scintilla *scintilla_new(void (*callback)(Scintilla *, int, void *, void *)) {
  init_colors();
  return reinterpret_cast<Scintilla *>(new ScintillaTerm(callback));
}
/**
 * Returns the curses `WINDOW` associated with the given Scintilla window.
 * @param sci The Scintilla window returned by `scintilla_new()`.
 * @return curses `WINDOW`.
 */
WINDOW *scintilla_get_window(Scintilla *sci) {
  return reinterpret_cast<ScintillaTerm *>(sci)->GetWINDOW();
}
/**
 * Sends the given message with parameters to the given Scintilla window.
 * @param sci The Scintilla window returned by `scintilla_new()`.
 * @param iMessage The message ID.
 * @param wParam The first parameter.
 * @param lParam The second parameter.
 */
sptr_t scintilla_send_message(Scintilla *sci, unsigned int iMessage,
                              uptr_t wParam, sptr_t lParam) {
  return reinterpret_cast<ScintillaTerm *>(sci)->WndProc(iMessage, wParam,
                                                         lParam);
}
/**
 * Sends the specified key to the given Scintilla window for processing.
 * If it is not consumed, an SCNotification will be emitted.
 * @param sci The Scintilla window returned by `scintilla_new()`.
 * @param key The keycode of the key.
 * @param shift Flag indicating whether or not the shift modifier key is
 *   pressed.
 * @param ctrl Flag indicating whether or not the control modifier key is
 *   pressed.
 * @param alt Flag indicating whether or not the alt modifier key is pressed.
 */
void scintilla_send_key(Scintilla *sci, int key, bool shift, bool ctrl,
                        bool alt) {
  reinterpret_cast<ScintillaTerm *>(sci)->KeyPress(key, shift, ctrl, alt);
}
/**
 * Copies the text of Scintilla's internal clipboard, not the primary and/or
 * secondary X selections, into the given buffer and returns the size of the
 * clipboard text.
 * Call with a `null` buffer first to get the size of the buffer needed to store
 * clipboard text.
 * Keep in mind clipboard text may contain null bytes.
 * @param sci The Scintilla window returned by `scintilla_new()`.
 * @param buffer The buffer to copy clipboard text to.
 * @return size of the clipboard text.
 */
int scintilla_get_clipboard(Scintilla *sci, char *buffer) {
  return reinterpret_cast<ScintillaTerm *>(sci)->GetClipboard(buffer);
}
/**
 * Refreshes the Scintilla window.
 * This should be done along with the normal curses `refresh()`.
 * @param sci The Scintilla window returned by `scintilla_new()`.
 */
void scintilla_refresh(Scintilla *sci) {
  reinterpret_cast<ScintillaTerm *>(sci)->Refresh();
}
/**
 * Deletes the given Scintilla window.
 * This function does not delete the curses `WINDOW` associated with it. You
 * will have to delete the `WINDOW` manually.
 * @param sci The Scintilla window returned by `scintilla_new()`.
 */
void scintilla_delete(Scintilla *sci) {
  delete reinterpret_cast<ScintillaTerm *>(sci);
}
}
