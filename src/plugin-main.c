/*
SWU Scan for OBS
Copyright (C) 2026 The SWU Scan authors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Shows the cards the SWU Scan app confirms (docs/obs-integration.md, the protocol shared with the iOS
 * and Android apps).
 *
 * Two halves:
 *  - the obs-websocket vendor "swuscan": the app, connected to OBS's built-in WebSocket server, calls
 *    `hello`, `show_card` and `clear` through CallVendorRequest. A vendor callback can't fail the request
 *    (obs-websocket reports success for any request a vendor handles), so errors go in the reply as
 *    {"error": "..."}.
 *  - two sources, placed apart in a scene: "SWU Scan Card" (data/card.html: the art, name and printing) and
 *    "SWU Scan Prices" (data/prices.html: the price lines). Each instance owns a private browser source
 *    showing its page with the fixed "Identified by SWU Scan" badge. The pages are part of the plugin. A
 *    source's properties (what shows, layout, colours) reach its page as custom properties in generated CSS
 *    (build_css), never as free-form CSS, so there is nothing that restyles or switches off the badge.
 *    (Anyone can still crop or cover a source in OBS — compositing can't be prevented — but removing the
 *    badge is deliberate.)
 *
 * How a card reaches the page: the plugin calls obs-browser's own `emit_event` vendor request through
 * obs-websocket's in-process API (obs_websocket_call_request), which dispatches a JavaScript CustomEvent
 * ("swuscanCard" / "swuscanClear") into browser sources. obs-browser sends it to every browser source; only
 * our two pages listen for those names, and each takes the part of the card it shows. A page that loads after the event (a new source, a scene switched
 * in) would miss it, so the last card is re-sent shortly after a source is created or shown; `seq` lets the
 * page ignore a re-send of the card it already shows.
 *
 * The card art arrives as base64 JPEG bytes from the phone, so the plugin never touches the network.
 */

#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <plugin-support.h>

#include "obs-websocket-api.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define SWUSCAN_PROTOCOL 1
#define SWUSCAN_VENDOR "swuscan"
#define CARD_SOURCE_ID "swuscan_card"
#define PRICES_SOURCE_ID "swuscan_prices"
/* How long after a source is created or shown the last card is re-sent: long enough for the page to load. */
#define RESYNC_DELAY_NS 1500000000ULL

enum source_kind { KIND_CARD, KIND_PRICES };

struct card_source {
	obs_source_t *self;
	obs_source_t *browser;
	enum source_kind kind;
	char *css; /* what build_css last gave the browser source */
	uint32_t width;
	uint32_t height;
	double auto_hide;   /* seconds; 0 = never */
	uint64_t resync_at; /* os_gettime_ns() when the last card should be re-sent; 0 = none pending */
	struct card_source *next;
};

/* Shared by every source and the vendor callbacks (which run on obs-websocket's threads). */
static pthread_mutex_t g_mutex;
static struct card_source *g_sources;
static char *g_last_card;   /* JSON of the last show_card (with "seq"), NULL after clear */
static uint64_t g_shown_at; /* os_gettime_ns() of the last show_card */
static long long g_seq;
static obs_websocket_vendor g_vendor;

/* ------------------------------------------------------------------ talking to the page */

/* Dispatch a JavaScript CustomEvent into browser sources via obs-browser's emit_event. */
static void emit_js(const char *event_name, obs_data_t *event_data)
{
	obs_data_t *inner = obs_data_create();
	obs_data_set_string(inner, "event_name", event_name);
	if (event_data)
		obs_data_set_obj(inner, "event_data", event_data);
	obs_data_t *req = obs_data_create();
	obs_data_set_string(req, "vendorName", "obs-browser");
	obs_data_set_string(req, "requestType", "emit_event");
	obs_data_set_obj(req, "requestData", inner);

	struct obs_websocket_request_response *resp = obs_websocket_call_request("CallVendorRequest", req);
	if (!resp)
		obs_log(LOG_WARNING, "obs-websocket isn't available; can't reach the card page");
	else if (resp->status_code != 100)
		obs_log(LOG_WARNING, "emit_event failed (%u): %s", resp->status_code,
			resp->comment ? resp->comment : "no comment");
	obs_websocket_request_response_free(resp);
	obs_data_release(req);
	obs_data_release(inner);
}

/* Re-send the last card (UI thread task; never from the graphics thread). */
static void resync_task(void *unused)
{
	UNUSED_PARAMETER(unused);
	pthread_mutex_lock(&g_mutex);
	obs_data_t *card = g_last_card ? obs_data_create_from_json(g_last_card) : NULL;
	pthread_mutex_unlock(&g_mutex);
	if (card) {
		emit_js("swuscanCard", card);
		obs_data_release(card);
	}
}

static int count_sources_locked(enum source_kind kind)
{
	int n = 0;
	for (struct card_source *s = g_sources; s; s = s->next)
		if (s->kind == kind)
			n++;
	return n;
}

/* ------------------------------------------------------------------ the vendor */

static bool protocol_ok(obs_data_t *request, obs_data_t *response)
{
	long long p = obs_data_get_int(request, "protocol");
	if (p <= SWUSCAN_PROTOCOL)
		return true;
	char msg[128];
	snprintf(msg, sizeof(msg), "protocol %lld not supported; update the SWU Scan OBS plugin", p);
	obs_data_set_string(response, "error", msg);
	return false;
}

static void vendor_hello(obs_data_t *request, obs_data_t *response, void *priv)
{
	UNUSED_PARAMETER(priv);
	obs_data_t *app = obs_data_get_obj(request, "app");
	if (app) {
		obs_log(LOG_INFO, "hello from SWU Scan %s %s", obs_data_get_string(app, "platform"),
			obs_data_get_string(app, "version"));
		obs_data_release(app);
	}
	pthread_mutex_lock(&g_mutex);
	int cards = count_sources_locked(KIND_CARD);
	int prices = count_sources_locked(KIND_PRICES);
	pthread_mutex_unlock(&g_mutex);
	obs_data_set_int(response, "protocol", SWUSCAN_PROTOCOL);
	obs_data_set_string(response, "pluginVersion", PLUGIN_VERSION);
	obs_data_set_int(response, "cardSources", cards);
	obs_data_set_int(response, "priceSources", prices);
}

static void vendor_show_card(obs_data_t *request, obs_data_t *response, void *priv)
{
	UNUSED_PARAMETER(priv);
	if (!protocol_ok(request, response))
		return;
	pthread_mutex_lock(&g_mutex);
	obs_data_set_int(request, "seq", ++g_seq);
	bfree(g_last_card);
	g_last_card = bstrdup(obs_data_get_json(request));
	g_shown_at = os_gettime_ns();
	int n = count_sources_locked(KIND_CARD) + count_sources_locked(KIND_PRICES);
	pthread_mutex_unlock(&g_mutex);

	emit_js("swuscanCard", request);
	obs_data_set_int(response, "shown", n);
}

static void vendor_clear(obs_data_t *request, obs_data_t *response, void *priv)
{
	UNUSED_PARAMETER(priv);
	if (!protocol_ok(request, response))
		return;
	pthread_mutex_lock(&g_mutex);
	bfree(g_last_card);
	g_last_card = NULL;
	pthread_mutex_unlock(&g_mutex);
	emit_js("swuscanClear", NULL);
}

/* ------------------------------------------------------------------ the source */

static const char *card_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SWUScanCard");
}

static const char *prices_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SWUScanPrices");
}

/* OBS colours are 0xAABBGGRR. */
static void css_color(struct dstr *css, const char *name, long long color)
{
	dstr_catf(css, "%s:rgba(%d,%d,%d,%.3f);", name, (int)(color & 0xff), (int)((color >> 8) & 0xff),
		  (int)((color >> 16) & 0xff), (double)((color >> 24) & 0xff) / 255.0);
}

static void css_flag(struct dstr *css, const char *name, bool on)
{
	dstr_catf(css, "%s:%d;", name, on ? 1 : 0);
}

/* The source's properties as the custom properties its page reads. Only numbers go in, so a setting can't
 * add a rule of its own. */
static char *build_css(enum source_kind kind, obs_data_t *settings)
{
	struct dstr css = {0};
	dstr_cat(&css, ":root{");
	css_color(&css, "--panel", obs_data_get_int(settings, "background"));
	css_color(&css, "--ink", obs_data_get_int(settings, "text"));
	css_color(&css, "--edge", obs_data_get_int(settings, "border"));
	css_flag(&css, "--vertical", obs_data_get_int(settings, "layout") == 1);
	if (kind == KIND_CARD) {
		css_flag(&css, "--show-art", obs_data_get_bool(settings, "show_art"));
		css_flag(&css, "--show-subtitle", obs_data_get_bool(settings, "show_subtitle"));
		css_flag(&css, "--show-printing", obs_data_get_bool(settings, "show_printing"));
	} else {
		css_flag(&css, "--show-name", obs_data_get_bool(settings, "show_name"));
		css_flag(&css, "--show-cardmarket", obs_data_get_bool(settings, "show_cardmarket"));
		css_flag(&css, "--show-tcgplayer", obs_data_get_bool(settings, "show_tcgplayer"));
	}
	dstr_cat(&css, "}");
	return css.array;
}

static void apply_browser_settings(struct card_source *s)
{
	obs_data_t *bs = obs_data_create();
	char *page = obs_module_file(s->kind == KIND_CARD ? "card.html" : "prices.html");
	obs_data_set_bool(bs, "is_local_file", true);
	obs_data_set_string(bs, "local_file", page ? page : "");
	obs_data_set_int(bs, "width", s->width);
	obs_data_set_int(bs, "height", s->height);
	obs_data_set_string(bs, "css", s->css); /* generated from the properties; no user CSS */
	obs_data_set_bool(bs, "shutdown", false);
	obs_data_set_bool(bs, "restart_when_active", false);
	obs_data_set_bool(bs, "reroute_audio", false);
	if (s->browser)
		obs_source_update(s->browser, bs);
	else
		s->browser = obs_source_create_private("browser_source", "SWU Scan page", bs);
	bfree(page);
	obs_data_release(bs);
}

static void card_update(void *data, obs_data_t *settings)
{
	struct card_source *s = data;
	uint32_t w = (uint32_t)obs_data_get_int(settings, "width");
	uint32_t h = (uint32_t)obs_data_get_int(settings, "height");
	s->auto_hide = obs_data_get_double(settings, "auto_hide");
	char *css = build_css(s->kind, settings);
	if (w != s->width || h != s->height || !s->browser || !s->css || strcmp(css, s->css) != 0) {
		s->width = w;
		s->height = h;
		bfree(s->css);
		s->css = css;
		apply_browser_settings(s);
		s->resync_at = os_gettime_ns() + RESYNC_DELAY_NS; /* a changed page reloads */
	} else {
		bfree(css);
	}
}

static void *source_create(obs_data_t *settings, obs_source_t *source, enum source_kind kind)
{
	struct card_source *s = bzalloc(sizeof(*s));
	s->self = source;
	s->kind = kind;
	card_update(s, settings);
	if (!s->browser)
		obs_log(LOG_WARNING, "couldn't create the browser source; is obs-browser installed?");
	pthread_mutex_lock(&g_mutex);
	s->next = g_sources;
	g_sources = s;
	pthread_mutex_unlock(&g_mutex);
	return s;
}

static void *card_create(obs_data_t *settings, obs_source_t *source)
{
	return source_create(settings, source, KIND_CARD);
}

static void *prices_create(obs_data_t *settings, obs_source_t *source)
{
	return source_create(settings, source, KIND_PRICES);
}

static void card_destroy(void *data)
{
	struct card_source *s = data;
	pthread_mutex_lock(&g_mutex);
	for (struct card_source **p = &g_sources; *p; p = &(*p)->next) {
		if (*p == s) {
			*p = s->next;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	obs_source_release(s->browser);
	bfree(s->css);
	bfree(s);
}

static void common_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "auto_hide", 0.0);
	obs_data_set_default_int(settings, "layout", 0);
	obs_data_set_default_int(settings, "background", 0xEB1A1412); /* rgba(18, 20, 26, 0.92) */
	obs_data_set_default_int(settings, "text", 0xFFEAF1F4);       /* #f4f1ea */
	obs_data_set_default_int(settings, "border", 0xFF4AB0E8);     /* #e8b04a */
}

static void card_defaults(obs_data_t *settings)
{
	common_defaults(settings);
	obs_data_set_default_int(settings, "width", 1000);
	obs_data_set_default_int(settings, "height", 300);
	obs_data_set_default_bool(settings, "show_art", true);
	obs_data_set_default_bool(settings, "show_subtitle", true);
	obs_data_set_default_bool(settings, "show_printing", true);
}

static void prices_defaults(obs_data_t *settings)
{
	common_defaults(settings);
	obs_data_set_default_int(settings, "width", 640);
	obs_data_set_default_int(settings, "height", 240);
	obs_data_set_default_bool(settings, "show_name", true);
	obs_data_set_default_bool(settings, "show_cardmarket", true);
	obs_data_set_default_bool(settings, "show_tcgplayer", true);
}

static obs_properties_t *common_properties(void)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "width", obs_module_text("Width"), 320, 3840, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 120, 2160, 1);
	obs_property_t *p = obs_properties_add_float(props, "auto_hide", obs_module_text("AutoHide"), 0.0, 600.0, 0.5);
	obs_property_float_set_suffix(p, " s");
	obs_property_set_long_description(p, obs_module_text("AutoHide.Description"));
	obs_property_t *layout = obs_properties_add_list(props, "layout", obs_module_text("Layout"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(layout, obs_module_text("Layout.Horizontal"), 0);
	obs_property_list_add_int(layout, obs_module_text("Layout.Vertical"), 1);
	return props;
}

static void add_colors(obs_properties_t *props)
{
	obs_properties_add_color_alpha(props, "background", obs_module_text("Background"));
	obs_properties_add_color_alpha(props, "text", obs_module_text("Text"));
	obs_properties_add_color_alpha(props, "border", obs_module_text("Border"));
}

static obs_properties_t *card_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = common_properties();
	obs_properties_add_bool(props, "show_art", obs_module_text("ShowArt"));
	obs_properties_add_bool(props, "show_subtitle", obs_module_text("ShowSubtitle"));
	obs_properties_add_bool(props, "show_printing", obs_module_text("ShowPrinting"));
	add_colors(props);
	return props;
}

static obs_properties_t *prices_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = common_properties();
	obs_properties_add_bool(props, "show_name", obs_module_text("ShowName"));
	obs_properties_add_bool(props, "show_cardmarket", obs_module_text("ShowCardmarket"));
	obs_properties_add_bool(props, "show_tcgplayer", obs_module_text("ShowTCGplayer"));
	add_colors(props);
	return props;
}

static uint32_t card_width(void *data)
{
	return ((struct card_source *)data)->width;
}

static uint32_t card_height(void *data)
{
	return ((struct card_source *)data)->height;
}

/* Whether this source draws the card right now: one has been shown and its auto-hide hasn't run out. */
static bool card_visible(struct card_source *s)
{
	pthread_mutex_lock(&g_mutex);
	bool has = g_last_card != NULL;
	uint64_t at = g_shown_at;
	pthread_mutex_unlock(&g_mutex);
	if (!has)
		return false;
	return s->auto_hide <= 0.0 || (double)(os_gettime_ns() - at) / 1e9 < s->auto_hide;
}

static void card_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct card_source *s = data;
	if (s->browser && card_visible(s))
		obs_source_video_render(s->browser);
}

static void card_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct card_source *s = data;
	if (s->resync_at && os_gettime_ns() >= s->resync_at) {
		s->resync_at = 0;
		obs_queue_task(OBS_TASK_UI, resync_task, NULL, false);
	}
}

/* The private browser only renders while something holds it active and showing: forward ours. */
static void card_activate(void *data)
{
	struct card_source *s = data;
	if (s->browser)
		obs_source_inc_active(s->browser);
}

static void card_deactivate(void *data)
{
	struct card_source *s = data;
	if (s->browser)
		obs_source_dec_active(s->browser);
}

static void card_show(void *data)
{
	struct card_source *s = data;
	if (s->browser)
		obs_source_inc_showing(s->browser);
	s->resync_at = os_gettime_ns() + RESYNC_DELAY_NS;
}

static void card_hide(void *data)
{
	struct card_source *s = data;
	if (s->browser)
		obs_source_dec_showing(s->browser);
}

static void card_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct card_source *s = data;
	if (s->browser)
		cb(s->self, s->browser, param);
}

static struct obs_source_info card_source_info = {
	.id = CARD_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type = OBS_ICON_TYPE_BROWSER,
	.get_name = card_get_name,
	.create = card_create,
	.destroy = card_destroy,
	.update = card_update,
	.get_defaults = card_defaults,
	.get_properties = card_properties,
	.get_width = card_width,
	.get_height = card_height,
	.video_render = card_render,
	.video_tick = card_tick,
	.activate = card_activate,
	.deactivate = card_deactivate,
	.show = card_show,
	.hide = card_hide,
	.enum_active_sources = card_enum_sources,
};

/* The same source with its own name, page and properties. */
static struct obs_source_info prices_source_info = {
	.id = PRICES_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type = OBS_ICON_TYPE_BROWSER,
	.get_name = prices_get_name,
	.create = prices_create,
	.destroy = card_destroy,
	.update = card_update,
	.get_defaults = prices_defaults,
	.get_properties = prices_properties,
	.get_width = card_width,
	.get_height = card_height,
	.video_render = card_render,
	.video_tick = card_tick,
	.activate = card_activate,
	.deactivate = card_deactivate,
	.show = card_show,
	.hide = card_hide,
	.enum_active_sources = card_enum_sources,
};

/* ------------------------------------------------------------------ module */

bool obs_module_load(void)
{
	pthread_mutex_init(&g_mutex, NULL);
	obs_register_source(&card_source_info);
	obs_register_source(&prices_source_info);
	obs_log(LOG_INFO, "plugin loaded (version %s, protocol %d)", PLUGIN_VERSION, SWUSCAN_PROTOCOL);
	return true;
}

/* obs-websocket loads as a module too, so its API is only there once every module has loaded. */
void obs_module_post_load(void)
{
	g_vendor = obs_websocket_register_vendor(SWUSCAN_VENDOR);
	if (!g_vendor) {
		obs_log(LOG_ERROR, "obs-websocket isn't available (OBS 28 or later with WebSocket is needed)");
		return;
	}
	if (!obs_websocket_vendor_register_request(g_vendor, "hello", vendor_hello, NULL) ||
	    !obs_websocket_vendor_register_request(g_vendor, "show_card", vendor_show_card, NULL) ||
	    !obs_websocket_vendor_register_request(g_vendor, "clear", vendor_clear, NULL))
		obs_log(LOG_ERROR, "couldn't register the swuscan vendor requests");
}

void obs_module_unload(void)
{
	bfree(g_last_card);
	g_last_card = NULL;
	pthread_mutex_destroy(&g_mutex);
	obs_log(LOG_INFO, "plugin unloaded");
}
