/*
 * TkActivity.java --
 *
 *	The activity of Tk on Android: a NativeActivity (whose native side is
 *	tkAndroidMain.c) with what cannot be done from native code, the input
 *	of text from the soft keyboard. Android only shows the keyboard for a
 *	view which edits text and gives the keyboard an InputConnection, and
 *	the keyboard sends most text through it rather than as key events: an
 *	invisible view is added for that, which takes the focus when Tk wants
 *	the keyboard (tkKeyboard.c), tells the keyboard the type of the text,
 *	and gives the text the keyboard commits to Tk. Tk is also told when
 *	the keyboard is shown or hidden.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

package tk.android;

import android.app.NativeActivity;
import android.content.Context;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

public class TkActivity extends NativeActivity {

    /*
     * Implemented by tkAndroidMain.c: give Tk the text the keyboard
     * committed, or the number of characters to delete before and after the
     * insertion cursor.
     */

    static native void nativeCommitText(String text);
    static native void nativeDeleteText(int before, int after);
    static native void nativeKeyboardChanged(boolean shown);

    /*
     * The types of text of tkKeyboard.c (TK_KEYBOARD_*).
     */

    static final int TYPE_TEXT = 0;
    static final int TYPE_NUMBER = 1;
    static final int TYPE_PHONE = 2;
    static final int TYPE_EMAIL = 3;
    static final int TYPE_URL = 4;
    static final int TYPE_PASSWORD = 5;

    private InputView inputView;
    private boolean keyboardShown = false;

    /*
     * The view the keyboard edits. It is 1 by 1 pixel, and draws nothing.
     */

    static class InputView extends View {
	int type = TYPE_TEXT;		/* The type of the text. */

	InputView(Context context) {
	    super(context);
	    setFocusable(true);
	    setFocusableInTouchMode(true);
	}

	@Override
	public boolean onCheckIsTextEditor() {
	    return true;
	}

	@Override
	public InputConnection onCreateInputConnection(EditorInfo info) {
	    /*
	     * Without suggestions, keyboards commit each character as it is
	     * typed, instead of composing words.
	     */

	    switch (type) {
	    case TYPE_NUMBER:
		info.inputType = InputType.TYPE_CLASS_NUMBER
			| InputType.TYPE_NUMBER_FLAG_SIGNED
			| InputType.TYPE_NUMBER_FLAG_DECIMAL;
		break;
	    case TYPE_PHONE:
		info.inputType = InputType.TYPE_CLASS_PHONE;
		break;
	    case TYPE_EMAIL:
		info.inputType = InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS
			| InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
		break;
	    case TYPE_URL:
		info.inputType = InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_URI
			| InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
		break;
	    case TYPE_PASSWORD:
		info.inputType = InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_PASSWORD;
		break;
	    default:
		info.inputType = InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
			| InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
		break;
	    }
	    info.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN
		    | EditorInfo.IME_FLAG_NO_EXTRACT_UI
		    | EditorInfo.IME_ACTION_NONE;
	    return new BaseInputConnection(this, false) {
		/*
		 * Text being composed is inserted, and replaced as it changes:
		 * the number of characters of it which are in the widget.
		 */

		private int composing = 0;

		private void replaceComposing(CharSequence text) {
		    if (composing > 0) {
			nativeDeleteText(composing, 0);
		    }
		    composing = 0;
		    if (text.length() > 0) {
			nativeCommitText(text.toString());
		    }
		}

		@Override
		public boolean commitText(CharSequence text, int position) {
		    replaceComposing(text);
		    return true;
		}

		@Override
		public boolean setComposingText(CharSequence text,
			int position) {
		    replaceComposing(text);
		    composing = Character.codePointCount(text, 0, text.length());
		    return true;
		}

		@Override
		public boolean finishComposingText() {
		    composing = 0;
		    return true;
		}

		@Override
		public boolean deleteSurroundingText(int before, int after) {
		    nativeDeleteText(before, after);
		    return true;
		}
	    };
	}
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
	super.onCreate(savedInstanceState);
	inputView = new InputView(this);
	addContentView(inputView, new ViewGroup.LayoutParams(1, 1));

	/*
	 * Tell Tk when the keyboard is shown or hidden, by Tk or by the user.
	 */

	getWindow().getDecorView().setOnApplyWindowInsetsListener(
		new View.OnApplyWindowInsetsListener() {
	    @Override
	    public WindowInsets onApplyWindowInsets(View view,
		    WindowInsets insets) {
		boolean shown = insets.isVisible(WindowInsets.Type.ime());

		if (shown != keyboardShown) {
		    keyboardShown = shown;
		    nativeKeyboardChanged(shown);
		}
		return view.onApplyWindowInsets(insets);
	    }
	});
    }

    /*
     * Called by Tk, from its thread, to show or hide the keyboard, for text
     * of a type.
     */

    public void showKeyboard(final boolean show, final int type) {
	runOnUiThread(new Runnable() {
	    @Override
	    public void run() {
		InputMethodManager imm = (InputMethodManager)
			getSystemService(Context.INPUT_METHOD_SERVICE);

		if (show) {
		    boolean restart = (inputView.type != type);

		    inputView.type = type;
		    inputView.requestFocus();
		    if (restart) {
			imm.restartInput(inputView);
		    }
		    imm.showSoftInput(inputView, 0);
		} else {
		    imm.hideSoftInputFromWindow(inputView.getWindowToken(), 0);
		}
	    }
	});
    }
}
