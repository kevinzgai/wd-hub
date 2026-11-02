package com.xiaozhi.wbboard;

import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.view.KeyEvent;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.webkit.WebViewAssetLoader;

/**
 * WORKBUDDY 看板 · 轻壳
 *
 * 页面和 /api 都在电脑上跑，这里只是把它装进一个没有地址栏的窗口。
 * 地址存在 SharedPreferences，连不上会自动回到配置页，不用重新打包。
 */
public class MainActivity extends AppCompatActivity {

    private static final String PREFS = "wbboard";
    private static final String KEY_URL = "server_url";
    private static final String DEFAULT_PORT = "8000";
    private static final String HOST = "https://appassets.androidplatform.net";
    private static final String SETUP = HOST + "/assets/setup.html";

    private WebView web;
    private WebViewAssetLoader assetLoader;
    private boolean longPressed = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().setBackgroundDrawable(new ColorDrawable(Color.parseColor("#0b0e13")));
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        WindowInsetsControllerCompat ctl =
                WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
        if (ctl != null) {
            ctl.setAppearanceLightStatusBars(false);
            ctl.setAppearanceLightNavigationBars(false);
        }

        assetLoader = new WebViewAssetLoader.Builder()
                .addPathHandler("/assets/", new WebViewAssetLoader.AssetsPathHandler(this))
                .build();

        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#0b0e13"));
        setContentView(web);

        ViewCompat.setOnApplyWindowInsetsListener(web, (v, insets) -> {
            Insets bars = insets.getInsets(
                    WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
            v.setPadding(0, bars.top, 0, bars.bottom);
            return insets;
        });

        configure(web);

        String saved = prefs().getString(KEY_URL, null);
        if (saved == null || saved.trim().isEmpty()) {
            openSetup(null);
        } else {
            web.loadUrl(saved);
        }
    }

    private void configure(WebView view) {
        WebSettings s = view.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        // 看板每 2 秒轮询一次，页面本身不缓存，避免看到旧数据
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        s.setUseWideViewPort(true);
        s.setLoadWithOverviewMode(true);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);
        s.setDisplayZoomControls(false);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);

        view.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView v, WebResourceRequest req) {
                return assetLoader.shouldInterceptRequest(req.getUrl());
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest req) {
                Uri uri = req.getUrl();
                if ("wbboard".equals(uri.getScheme())) {
                    onNativeCommand(uri);
                    return true;
                }
                return false;
            }

            @Override
            public void onReceivedError(WebView v, WebResourceRequest req, WebResourceError err) {
                if (req.isForMainFrame()) {
                    String reason = err == null ? "" : String.valueOf(err.getDescription());
                    openSetup("连不上 " + req.getUrl() + "\n" + reason);
                }
            }
        });
    }

    private void onNativeCommand(Uri uri) {
        String host = uri.getHost();
        if ("save".equals(host)) {
            String raw = uri.getQueryParameter("url");
            String url = normalize(raw);
            if (url == null) {
                openSetup("地址格式不对，应该像 192.168.1.10:8000");
                return;
            }
            prefs().edit().putString(KEY_URL, url).apply();
            web.loadUrl(url);
            return;
        }
        if ("reset".equals(host)) {
            prefs().edit().remove(KEY_URL).apply();
            openSetup("已清除保存的地址");
            return;
        }
        if ("reload".equals(host)) {
            String saved = prefs().getString(KEY_URL, null);
            if (saved != null) web.loadUrl(saved);
        }
    }

    /** 补齐协议头和默认端口，让用户只输 IP 也能用。 */
    private String normalize(String raw) {
        if (raw == null) return null;
        String v = raw.trim();
        if (v.isEmpty()) return null;
        if (!v.startsWith("http://") && !v.startsWith("https://")) v = "http://" + v;
        String tail = v.substring(v.indexOf("://") + 3);
        if (tail.endsWith("/")) tail = tail.substring(0, tail.length() - 1);
        int slash = tail.indexOf('/');
        String authority = slash >= 0 ? tail.substring(0, slash) : tail;
        String rest = slash >= 0 ? tail.substring(slash) : "";
        if (!authority.contains(":")) authority = authority + ":" + DEFAULT_PORT;
        if (authority.replace(":", "").replace(DEFAULT_PORT, "").isEmpty()) return null;
        return v.substring(0, v.indexOf("://") + 3) + authority + rest;
    }

    private void openSetup(String message) {
        String url = SETUP;
        if (message != null && !message.isEmpty()) {
            url = url + "?msg=" + Uri.encode(message);
        }
        String saved = prefs().getString(KEY_URL, null);
        if (saved != null) url = url + (url.contains("?") ? "&" : "?") + "cur=" + Uri.encode(saved);
        web.loadUrl(url);
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            event.startTracking();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyLongPress(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            longPressed = true;
            openSetup("长按返回键进入设置");
            return true;
        }
        return super.onKeyLongPress(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            if (longPressed) {
                longPressed = false;
                return true;
            }
            String now = web.getUrl();
            if (now != null && now.startsWith(SETUP)) {
                finish();
            } else if (web.canGoBack()) {
                web.goBack();
            } else {
                finish();
            }
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }
}
