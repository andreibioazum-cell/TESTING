package com.luac.studio;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

public final class CodeEditorView extends View {
    private static native void nativeSetText(String text);
    private static native String nativeGetText();
    private static native void nativeInsert(String text);
    private static native void nativeNewline();
    private static native void nativeBackspace();
    private static native void nativeDelete();
    private static native void nativeSetCursor(int line, int col);
    private static native void nativeMoveCursor(int dx, int dy);
    private static native int nativeLineCount();
    private static native int nativeCursorLine();
    private static native int nativeCursorCol();
    private static native String nativeGetLine(int line);
    private static native int[] nativeTokenizeLine(int line);
    private static native boolean nativeUndo();
    private static native boolean nativeRedo();

    private static final int[] TOKEN_COLORS = {
        0xFFF8FAFC, // 0 PLAIN
        0xFFC084FC, // 1 KEYWORD
        0xFF22D3EE, // 2 TYPE
        0xFFFBBF24, // 3 BUILTIN
        0xFF4ADE80, // 4 STRING
        0xFFFB923C, // 5 NUMBER
        0xFF64748B, // 6 COMMENT
        0xFF94A3B8  // 7 OPERATOR
    };

    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint gutterPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bgPaint = new Paint();
    private final Handler blinkHandler = new Handler(Looper.getMainLooper());

    private float lineHeight;
    private float charWidth;
    private float gutterWidth;
    private float padX, padY;
    private float scrollX, scrollY;
    private float downX, downY, lastX, lastY;
    private boolean dragging;
    private final int touchSlop;
    private boolean cursorVisible = true;
    private boolean nativeReady = true;
    private final StringBuilder fallbackText = new StringBuilder();

    private final Runnable blinkTask = new Runnable() {
        @Override public void run() {
            if (hasFocus()) {
                cursorVisible = !cursorVisible;
                invalidate();
            }
            blinkHandler.postDelayed(this, 530);
        }
    };

    public CodeEditorView(Context context) { this(context, null); }
    public CodeEditorView(Context context, AttributeSet attrs) {
        super(context, attrs);
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        float density = context.getResources().getDisplayMetrics().density;
        padX = 10f * density;
        padY = 10f * density;
        textPaint.setTypeface(Typeface.MONOSPACE);
        textPaint.setTextSize(14f * density);
        gutterPaint.setTypeface(Typeface.MONOSPACE);
        gutterPaint.setTextSize(12f * density);
        gutterPaint.setTextAlign(Paint.Align.RIGHT);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        lineHeight = (float) Math.ceil(fm.descent - fm.ascent + 4f * density);
        charWidth = textPaint.measureText("M");
        gutterWidth = 40f * density;
        setFocusable(true);
        setFocusableInTouchMode(true);
        try { nativeLineCount(); } catch (UnsatisfiedLinkError e) { nativeReady = false; }
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        blinkHandler.removeCallbacks(blinkTask);
        blinkHandler.postDelayed(blinkTask, 530);
    }

    @Override protected void onDetachedFromWindow() {
        blinkHandler.removeCallbacks(blinkTask);
        super.onDetachedFromWindow();
    }

    public void setText(String text) {
        String value = text == null ? "" : text;
        if (nativeReady) nativeSetText(value);
        else { fallbackText.setLength(0); fallbackText.append(value); }
        scrollX = 0;
        scrollY = 0;
        cursorVisible = true;
        invalidate();
    }

    public String getTextString() {
        return nativeReady ? nativeGetText() : fallbackText.toString();
    }

    public void insertSnippet(String snippet) {
        if (snippet == null) return;
        if (nativeReady) nativeInsert(snippet);
        else fallbackText.append(snippet);
        ensureCursorVisible();
        invalidate();
    }

    public void undo() {
        if (nativeReady && nativeUndo()) {
            ensureCursorVisible();
            invalidate();
        }
    }

    public void redo() {
        if (nativeReady && nativeRedo()) {
            ensureCursorVisible();
            invalidate();
        }
    }

    private void ensureCursorVisible() {
        if (!nativeReady) return;
        int line = nativeCursorLine();
        int col = nativeCursorCol();
        int totalLines = Math.max(1, nativeLineCount());
        float viewW = Math.max(1, getWidth() - gutterWidth - padX);
        float viewH = Math.max(1, getHeight());
        float curY = padY + line * lineHeight;
        float curX = col * charWidth;

        if (curY < scrollY + padY) scrollY = Math.max(0, curY - padY);
        else if (curY + lineHeight > scrollY + viewH - padY)
            scrollY = Math.max(0, curY + lineHeight - viewH + padY);

        if (curX < scrollX) scrollX = Math.max(0, curX - charWidth * 4);
        else if (curX > scrollX + viewW - charWidth * 2)
            scrollX = Math.max(0, curX - viewW + charWidth * 4);

        float maxScrollY = Math.max(0, totalLines * lineHeight + padY * 2 - viewH);
        if (scrollY > maxScrollY) scrollY = maxScrollY;
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        int h = getHeight();
        bgPaint.setColor(0xFF0B1220);
        canvas.drawRect(0, 0, w, h, bgPaint);

        if (!nativeReady) {
            textPaint.setColor(0xFFF8FAFC);
            canvas.drawText(fallbackText.toString(), padX, padY + lineHeight, textPaint);
            return;
        }

        int lineCount = Math.max(1, nativeLineCount());
        int curLine = nativeCursorLine();
        int curCol = nativeCursorCol();
        int digits = Math.max(2, String.valueOf(lineCount).length());
        gutterWidth = Math.max(32f, (digits + 1.5f) * charWidth);

        int firstLine = Math.max(0, (int) ((scrollY - padY) / lineHeight));
        int lastLine = Math.min(lineCount - 1, (int) ((scrollY + h) / lineHeight) + 1);
        float baselineOffset = -textPaint.getFontMetrics().ascent;

        /* Active line highlight */
        float activeTop = padY + curLine * lineHeight - scrollY;
        if (activeTop + lineHeight >= 0 && activeTop <= h) {
            bgPaint.setColor(0xFF162033);
            canvas.drawRect(gutterWidth, activeTop, w, activeTop + lineHeight, bgPaint);
        }

        /* Code area clipped to right of gutter */
        canvas.save();
        canvas.clipRect(gutterWidth, 0, w, h);
        for (int line = firstLine; line <= lastLine; ++line) {
            String text = nativeGetLine(line);
            int[] tokens = nativeTokenizeLine(line);
            float y = padY + line * lineHeight - scrollY + baselineOffset;
            float xStart = gutterWidth + padX - scrollX;
            drawHighlightedLine(canvas, text, tokens, xStart, y);
        }

        if (hasFocus() && cursorVisible) {
            float cx = gutterWidth + padX + curCol * charWidth - scrollX;
            float cy = padY + curLine * lineHeight - scrollY;
            bgPaint.setColor(0xFF38BDF8);
            canvas.drawRect(cx, cy + 2f, cx + Math.max(2f, charWidth * 0.14f), cy + lineHeight - 2f, bgPaint);
        }
        canvas.restore();

        /* Gutter background + line numbers */
        bgPaint.setColor(0xFF0F172A);
        canvas.drawRect(0, 0, gutterWidth, h, bgPaint);
        bgPaint.setColor(0xFF1E293B);
        canvas.drawRect(gutterWidth - 1.5f, 0, gutterWidth, h, bgPaint);

        for (int line = firstLine; line <= lastLine; ++line) {
            float y = padY + line * lineHeight - scrollY + baselineOffset;
            gutterPaint.setColor(line == curLine ? 0xFFE2E8F0 : 0xFF64748B);
            canvas.drawText(String.valueOf(line + 1), gutterWidth - padX * 0.5f, y, gutterPaint);
        }
    }

    private void drawHighlightedLine(Canvas canvas, String text, int[] tokens, float xStart, float y) {
        if (text == null || text.isEmpty()) return;
        int len = text.length();
        int pos = 0;
        float x = xStart;
        if (tokens != null) {
            for (int i = 0; i + 2 < tokens.length; i += 3) {
                int start = Math.max(pos, Math.min(len, tokens[i]));
                int end = Math.max(start, Math.min(len, start + tokens[i + 1]));
                int kind = tokens[i + 2];
                if (start > pos) {
                    textPaint.setColor(TOKEN_COLORS[0]);
                    canvas.drawText(text, pos, start, x, y, textPaint);
                    x += textPaint.measureText(text, pos, start);
                }
                if (end > start) {
                    int color = (kind >= 0 && kind < TOKEN_COLORS.length) ? TOKEN_COLORS[kind] : TOKEN_COLORS[0];
                    textPaint.setColor(color);
                    canvas.drawText(text, start, end, x, y, textPaint);
                    x += textPaint.measureText(text, start, end);
                }
                pos = end;
            }
        }
        if (pos < len) {
            textPaint.setColor(TOKEN_COLORS[0]);
            canvas.drawText(text, pos, len, x, y, textPaint);
        }
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        float x = event.getX();
        float y = event.getY();
        if (action == MotionEvent.ACTION_DOWN) {
            downX = lastX = x;
            downY = lastY = y;
            dragging = false;
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            float dx = x - lastX;
            float dy = y - lastY;
            if (!dragging && (Math.abs(x - downX) > touchSlop || Math.abs(y - downY) > touchSlop)) {
                dragging = true;
            }
            if (dragging && nativeReady) {
                int lineCount = Math.max(1, nativeLineCount());
                float maxY = Math.max(0, lineCount * lineHeight + padY * 2 - getHeight());
                scrollX = Math.max(0, scrollX - dx);
                scrollY = Math.max(0, Math.min(maxY, scrollY - dy));
                invalidate();
            }
            lastX = x;
            lastY = y;
            return true;
        }
        if (action == MotionEvent.ACTION_UP && !dragging) {
            requestFocus();
            if (nativeReady) {
                int line = Math.max(0, (int) ((y + scrollY - padY) / lineHeight));
                int col = Math.max(0, Math.round((x + scrollX - gutterWidth - padX) / charWidth));
                nativeSetCursor(line, col);
                cursorVisible = true;
                invalidate();
            }
            InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
            return true;
        }
        return super.onTouchEvent(event);
    }

    @Override public boolean onCheckIsTextEditor() { return true; }

    @Override public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_NONE;
        return new BaseInputConnection(this, false) {
            @Override public boolean commitText(CharSequence text, int newCursorPosition) {
                if (text != null && text.length() > 0) {
                    String s = text.toString();
                    if ("\n".equals(s)) {
                        if (nativeReady) nativeNewline();
                    } else if ("\t".equals(s)) {
                        if (nativeReady) nativeInsert("    ");
                    } else {
                        if (nativeReady) nativeInsert(s);
                        else fallbackText.append(s);
                    }
                    cursorVisible = true;
                    ensureCursorVisible();
                    invalidate();
                }
                return true;
            }

            @Override public boolean setComposingText(CharSequence text, int newCursorPosition) {
                return commitText(text, newCursorPosition);
            }

            @Override public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                if (nativeReady) {
                    for (int i = 0; i < beforeLength; ++i) nativeBackspace();
                    for (int i = 0; i < afterLength; ++i) nativeDelete();
                    cursorVisible = true;
                    ensureCursorVisible();
                    invalidate();
                }
                return true;
            }

            @Override public boolean sendKeyEvent(KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_DOWN) {
                    return onKeyDown(event.getKeyCode(), event);
                }
                return super.sendKeyEvent(event);
            }
        };
    }

    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (!nativeReady) return super.onKeyDown(keyCode, event);
        boolean handled = false;
        if (keyCode == KeyEvent.KEYCODE_DEL) { nativeBackspace(); handled = true; }
        else if (keyCode == KeyEvent.KEYCODE_FORWARD_DEL) { nativeDelete(); handled = true; }
        else if (keyCode == KeyEvent.KEYCODE_ENTER || keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER) { nativeNewline(); handled = true; }
        else if (keyCode == KeyEvent.KEYCODE_TAB) { nativeInsert("    "); handled = true; }
        else if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT) { nativeMoveCursor(-1, 0); handled = true; }
        else if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT) { nativeMoveCursor(1, 0); handled = true; }
        else if (keyCode == KeyEvent.KEYCODE_DPAD_UP) { nativeMoveCursor(0, -1); handled = true; }
        else if (keyCode == KeyEvent.KEYCODE_DPAD_DOWN) { nativeMoveCursor(0, 1); handled = true; }
        else {
            int ch = event.getUnicodeChar();
            if (ch > 0 && !Character.isISOControl(ch)) {
                nativeInsert(String.valueOf((char) ch));
                handled = true;
            }
        }
        if (handled) {
            cursorVisible = true;
            ensureCursorVisible();
            invalidate();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }
}
