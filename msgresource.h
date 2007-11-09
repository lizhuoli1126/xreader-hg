#ifndef MSGRESOURCE_H
#define MSGRESOURCE_H

typedef struct {
	const char* msg;
	int id;
} MsgResource;

enum {
	EXIT_PROMPT,
	YES,
	NO,
	BUTTON_PRESS_PROMPT,
	BUTTON_SETTING_PROMPT,
	BUTTON_SETTING_SEP,
	BOOKMARK_MENU_KEY,
	PREV_PAGE_KEY,
	NEXT_PAGE_KEY,
	PREV_100_LINE_KEY,
	NEXT_100_LINE_KEY,
	PREV_500_LINE_KEY,
	NEXT_500_LINE_KEY,
	TO_START_KEY,
	TO_END_KEY,
	NEXT_FILE,
	PREV_FILE,
	EXIT_KEY,
	FL_SELECTED,
	FL_FIRST,
	FL_LAST,
	FL_PARENT,
	FL_REFRESH,
	FL_FILE_OPS,
	FL_SELECT_FILE,
	IMG_OPT,
	IMG_OPT_CUBE,
	IMG_OPT_LINEAR,
	IMG_OPT_SECOND,
	IMG_OPT_ALGO,
	IMG_OPT_DELAY,
	IMG_OPT_START_POS,
	IMG_OPT_FLIP_SPEED,
	IMG_OPT_FLIP_MODE,
	IMG_OPT_REVERSE_WIDTH,
	IMG_OPT_THUMB_VIEW,
	IMG_OPT_BRIGHTNESS,
	TEXT_COLOR_OPT,
	NO_SUPPORT,
	FONT_COLOR,
	FONT_COLOR_RED,
	FONT_COLOR_GREEN,
	FONT_COLOR_BLUE,
	FONT_BACK_COLOR,
	FONT_COLOR_GRAY,
	FONT_BACK_GRAY,
	READ_OPTION,
	LEFT_DIRECT,
	RIGHT_DIRECT,
	REVERSAL_DIRECT,
	NONE_DIRECT,
	NEXT_ARTICLE,
	NO_ACTION,
	HAVE_SHUTDOWN,
	WORD_SPACE,
	LINE_SPACE,
	REVERSE_SPAN_SPACE,
	INFO_BAR_DISPLAY,
	REVERSE_ONE_LINE_WHEN_PAGE_DOWN,
	TEXT_DIRECTION,
	TEXT_ENCODING,
	SCROLLBAR,
	AUTOSAVE_BOOKMARK,
	TEXT_REALIGNMENT,
	TEXT_TAIL_PAGE_DOWN,
	AUTOPAGE_DELAY,
	AUTOLINE_STEP,
	LINE_CTRL_MODE,
	OPERATION_SETTING,
	CONTROL_PAGE,
	CONTROL_MUSIC,
	FONT_SETTING,
	MENU_FONT_SIZE,
	BOOK_FONT_SIZE,
	TTF_FONT_SIZE,
	UNKNOWN
};

const char* getmsgbyid(int id);
void setmsglang(const char* langname);

#endif
