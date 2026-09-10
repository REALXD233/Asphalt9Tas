package dev.a9tas.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Presentation only: retain the existing controls and their operation listeners. */
final class TasPageNavigation {
    private final Activity activity;
    private final LinearLayout[] pages = new LinearLayout[3];
    private final Button[] tabs = new Button[3];
    private final SharedPreferences preferences;
    private int selected;

    TasPageNavigation(Activity activity, boolean setupReady) {
        this.activity = activity;
        preferences = activity.getSharedPreferences("ui_navigation", Context.MODE_PRIVATE);
        LinearLayout navigation = activity.findViewById(R.id.pageNavigation);
        LinearLayout content = (LinearLayout) navigation.getParent();
        for (int i = 0; i < pages.length; i++) {
            pages[i] = new LinearLayout(activity);
            pages[i].setOrientation(LinearLayout.VERTICAL);
        }
        move(0, (View) activity.findViewById(R.id.recordButton).getParent());
        move(1, activity.findViewById(R.id.recordingInfoToggleButton));
        move(1, activity.findViewById(R.id.recordingInfoCard));
        move(1, (View) activity.findViewById(R.id.recordingFilterInput).getParent());
        move(2, activity.findViewById(R.id.licenseCard));
        move(2, activity.findViewById(R.id.prepareCard));
        move(2, activity.findViewById(R.id.advancedToggleButton));
        move(2, activity.findViewById(R.id.advancedCard));
        move(2, activity.findViewById(R.id.compatibilityFootnote));
        String[] names = {"刷圈", "录像", "设置"};
        for (int i = 0; i < pages.length; i++) {
            content.addView(pages[i], new LinearLayout.LayoutParams(-1, -2));
            final int page = i;
            Button tab = new Button(activity);
            tab.setText(names[i]);
            tab.setTextSize(15);
            tab.setAllCaps(false);
            tab.setMinHeight(dp(48));
            tab.setMinimumHeight(dp(48));
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, -2, 1);
            if (i > 0) params.leftMargin = dp(8);
            navigation.addView(tab, params);
            tabs[i] = tab;
            tab.setOnClickListener(view -> {
                view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
                View focused = activity.getCurrentFocus();
                if (focused != null) {
                    InputMethodManager input = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
                    if (input != null) input.hideSoftInputFromWindow(focused.getWindowToken(), 0);
                    focused.clearFocus();
                }
                select(page);
            });
        }
        select(setupReady ? preferences.getInt("page", 0) : 2);
        TextView status = activity.findViewById(R.id.statusText);
        activity.findViewById(R.id.statusDetailsButton).setOnClickListener(
                view -> showDetails("当前状态", status.getText()));
        TextView profile = activity.findViewById(R.id.profileText);
        profile.setMaxLines(3);
        profile.setEllipsize(android.text.TextUtils.TruncateAt.END);
        Button details = new Button(activity);
        details.setText("查看兼容性详情");
        details.setTextColor(activity.getResources().getColor(R.color.text_primary));
        details.setBackgroundResource(R.drawable.bg_button_secondary);
        details.setMinHeight(dp(48));
        details.setMinimumHeight(dp(48));
        LinearLayout parent = (LinearLayout) profile.getParent();
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(8);
        parent.addView(details, parent.indexOfChild(profile) + 1, params);
        details.setOnClickListener(view -> showDetails("兼容性详情", profile.getText()));
    }

    private void move(int page, View view) {
        ((ViewGroup) view.getParent()).removeView(view);
        pages[page].addView(view);
    }

    private void select(int page) {
        selected = Math.max(0, Math.min(2, page));
        for (int i = 0; i < pages.length; i++) {
            boolean active = i == selected;
            pages[i].setVisibility(active ? View.VISIBLE : View.GONE);
            tabs[i].setSelected(active);
            tabs[i].setContentDescription(tabs[i].getText() + (active ? "，当前页面" : ""));
            tabs[i].setTextColor(activity.getResources().getColor(R.color.text_primary));
            tabs[i].setBackgroundResource(active ? R.drawable.bg_button_primary : R.drawable.bg_button_secondary);
        }
        preferences.edit().putInt("page", selected).apply();
    }

    private void showDetails(String title, CharSequence text) {
        TextView body = new TextView(activity);
        body.setText(text);
        body.setTextSize(14);
        body.setTextColor(activity.getResources().getColor(R.color.text_primary));
        body.setTextIsSelectable(true);
        body.setPadding(dp(20), dp(16), dp(20), dp(16));
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(body);
        new AlertDialog.Builder(activity).setTitle(title).setView(scroll)
                .setPositiveButton("关闭", null).show();
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
