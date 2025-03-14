/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko.webapps;

import java.io.File;
import java.io.IOException;

import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.support.v7.widget.Toolbar;
import android.support.v7.app.ActionBar;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONObject;
import org.json.JSONException;

import org.mozilla.gecko.AppConstants;
import org.mozilla.gecko.EventDispatcher;
import org.mozilla.gecko.GeckoApp;
import org.mozilla.gecko.GeckoAppShell;
import org.mozilla.gecko.GeckoProfile;
import org.mozilla.gecko.icons.decoders.FaviconDecoder;
import org.mozilla.gecko.mozglue.SafeIntent;
import org.mozilla.gecko.R;
import org.mozilla.gecko.Tab;
import org.mozilla.gecko.Tabs;
import org.mozilla.gecko.util.ColorUtil;
import org.mozilla.gecko.util.EventCallback;
import org.mozilla.gecko.util.FileUtils;
import org.mozilla.gecko.util.GeckoBundle;

public class WebAppActivity extends GeckoApp {

    public static final String INTENT_KEY = "IS_A_WEBAPP";
    public static final String MANIFEST_PATH = "MANIFEST_PATH";

    private static final String LOGTAG = "WebAppActivity";

    private TextView mUrlView;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        loadManifest(getIntent());

        final Toolbar toolbar = (Toolbar) findViewById(R.id.actionbar);
        setSupportActionBar(toolbar);

        final ProgressBar progressBar = (ProgressBar) findViewById(R.id.page_progress);
        progressBar.setVisibility(View.GONE);

        final ActionBar actionBar = getSupportActionBar();
        actionBar.setCustomView(R.layout.webapps_action_bar_custom_view);
        actionBar.setDisplayShowCustomEnabled(true);
        actionBar.setDisplayShowTitleEnabled(false);
        actionBar.hide();

        final View customView = actionBar.getCustomView();
        mUrlView = (TextView) customView.findViewById(R.id.webapps_action_bar_url);

        EventDispatcher.getInstance().registerUiThreadListener(this,
                    "Website:AppEntered",
                    "Website:AppLeft",
                    null);

        Tabs.registerOnTabsChangedListener(this);
    }

    @Override
    public int getLayout() {
        return R.layout.customtabs_activity;
    }

    @Override
    public void handleMessage(final String event, final GeckoBundle message,
                              final EventCallback callback) {
        switch (event) {
            case "Website:AppEntered":
                getSupportActionBar().hide();
                break;

            case "Website:AppLeft":
                getSupportActionBar().show();
                break;
        }
    }

    @Override
    public void onTabChanged(Tab tab, Tabs.TabEvents msg, String data) {
        if (!Tabs.getInstance().isSelectedTab(tab)) {
            return;
        }

        if (msg == Tabs.TabEvents.LOCATION_CHANGE) {
            mUrlView.setText(tab.getURL());
        }
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        EventDispatcher.getInstance().unregisterUiThreadListener(this,
              "Website:AppEntered",
              "Website:AppLeft",
              null);
        Tabs.unregisterOnTabsChangedListener(this);
    }

    @Override
    protected int getNewTabFlags() {
        return Tabs.LOADURL_WEBAPP | super.getNewTabFlags();
    }

    /**
     * In case this activity is reused (the user has opened > 10 current web apps)
     * we check that app launched is still within the same host as the
     * shortcut has set, if not we reload the homescreens url
     */
    @Override
    protected void onNewIntent(Intent externalIntent) {

        restoreLastSelectedTab();

        final SafeIntent intent = new SafeIntent(externalIntent);
        final String launchUrl = intent.getDataString();
        final String currentUrl = Tabs.getInstance().getSelectedTab().getURL();
        final boolean isSameDomain = Uri.parse(currentUrl).getHost()
            .equals(Uri.parse(launchUrl).getHost());

        if (!isSameDomain) {
            loadManifest(externalIntent);
            Tabs.getInstance().loadUrl(launchUrl);
        }
    }

    private void loadManifest(Intent intent) {
        String manifestPath = intent.getStringExtra(WebAppActivity.MANIFEST_PATH);
        if (manifestPath != null) {
            updateFromManifest(manifestPath);
        }
    }

    private void updateFromManifest(String manifestPath) {
        try {
            final File manifestFile = new File(manifestPath);
            final JSONObject manifest = FileUtils.readJSONObjectFromFile(manifestFile);
            final JSONObject manifestField = (JSONObject) manifest.get("manifest");
            final Integer color = readColorFromManifest(manifestField);
            final String name = readNameFromManifest(manifestField);
            final Bitmap icon = readIconFromManifest(manifest);
            final ActivityManager.TaskDescription taskDescription = (color == null)
                ? new ActivityManager.TaskDescription(name, icon)
                : new ActivityManager.TaskDescription(name, icon, color);

            updateStatusBarColor(color);
            setTaskDescription(taskDescription);

        } catch (IOException | JSONException e) {
            Log.e(LOGTAG, "Failed to read manifest", e);
        }
    }

    private void updateStatusBarColor(final Integer themeColor) {
        if (themeColor != null && !AppConstants.Versions.preLollipop) {
            final Window window = getWindow();
            window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
            window.setStatusBarColor(ColorUtil.darken(themeColor, 0.25));
        }
    }

    private Integer readColorFromManifest(JSONObject manifest) throws JSONException {
        final String colorStr = (String) manifest.get("theme_color");
        if (colorStr != null) {
            return ColorUtil.parseStringColor(colorStr);
        }
        return null;
    }

    private String readNameFromManifest(JSONObject manifest) throws JSONException {
        String name = (String) manifest.get("name");
        if (name == null) {
            name = (String) manifest.get("short_name");
        }
        if (name == null) {
            name = (String) manifest.get("start_url");
        }
        return name;
    }

    private Bitmap readIconFromManifest(JSONObject manifest) throws JSONException {
        final String iconStr = (String) manifest.get("cached_icon");
        if (iconStr != null) {
            return FaviconDecoder
                .decodeDataURI(getContext(), iconStr)
                .getBestBitmap(GeckoAppShell.getPreferredIconSize());
        }
        return null;
    }
}
