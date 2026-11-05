/*
 * test-temp-storage-webkit.c - Temporary-container storage in real WebKit
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Regression test for the stray "1.blob" files: WebKitGTK 2.54 writes
 * IndexedDB Blob files of an *ephemeral* session to the relative path
 * "<n>.blob", i.e. into the process working directory, and every
 * database restarts the counter at 1, so the second blob fails with
 * EEXIST. Temporary containers now use a normal session over private
 * directories in $XDG_RUNTIME_DIR/gsurf/<process>/temp/<id>.
 *
 * Drives real WebKit views against a loopback page that stores a Blob
 * in IndexedDB and reads it back, and checks:
 *   - two temporary containers at once, several databases each, all
 *     store and read their blobs (no EEXIST);
 *   - nothing lands in the working directory;
 *   - the containers' data lives under the runtime directory, owner-only;
 *   - temporary containers do not see each other's IndexedDB data;
 *   - the directory is removed when the container is cleaned up.
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "test-util.h"
#include "webkit-test-util.h"

/* The page: put(db, key, text) stores a Blob and reads it back,
 * count(db) reports how many records a database holds (0 when new). */
static const gchar PAGE[] =
	"<html><body><script>"
	"function put(db,key,text){return new Promise(function(res,rej){"
	"var o=indexedDB.open(db,1);"
	"o.onupgradeneeded=function(){o.result.createObjectStore('s');};"
	"o.onerror=function(){rej('open:'+o.error);};"
	"o.onsuccess=function(){var d=o.result;var tx=d.transaction('s','readwrite');"
	"tx.objectStore('s').put(new Blob([text],{type:'text/plain'}),key);"
	"tx.onabort=function(){rej('abort:'+tx.error);};"
	"tx.oncomplete=function(){var g=d.transaction('s').objectStore('s').get(key);"
	"g.onerror=function(){rej('get:'+g.error);};"
	"g.onsuccess=function(){var b=g.result;"
	"if(!(b instanceof Blob)){rej('not a blob');return;}"
	"b.text().then(function(t){d.close();res(t);},function(e){rej('read:'+e);});};};};});}"
	"function count(db){return new Promise(function(res,rej){"
	"var o=indexedDB.open(db,1);"
	"o.onupgradeneeded=function(){o.result.createObjectStore('s');};"
	"o.onerror=function(){rej('open:'+o.error);};"
	"o.onsuccess=function(){var d=o.result;var c=d.transaction('s').objectStore('s').count();"
	"c.onsuccess=function(){d.close();res(String(c.result));};"
	"c.onerror=function(){rej('count:'+c.error);};};});}"
	"</script>idb</body></html>";

static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	(void)server;
	(void)path;
	(void)query;
	(void)data;
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_STATIC,
		PAGE, strlen(PAGE));
}

/* Run the promise expression @call in @view and return what it settled
 * to: "ok:<value>" or "err:<reason>". Bounded by the polling loop. */
static gchar *
settle(GsurfView *view, const gchar *call)
{
	WebKitWebView *native = gsurf_webkit_test_native(view);
	g_autofree gchar *start = g_strdup_printf(
		"window.__r=null;%s.then(function(v){window.__r='ok:'+v;},"
		"function(e){window.__r='err:'+e;});'started'", call);
	guint i;

	g_free(gsurf_webkit_test_eval(native, start));
	for (i = 0; i < 200; i++) {
		gchar *r = gsurf_webkit_test_eval(native, "String(window.__r)");

		if (g_strcmp0(r, "null") != 0)
			return r;
		g_free(r);
		gsurf_webkit_test_spin(25);
	}
	return g_strdup("err:timeout");
}

/* Number of "*.blob" entries directly in @dir. */
static guint
count_blobs(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	const gchar *name;
	guint n = 0;

	while (d != NULL && (name = g_dir_read_name(d)) != NULL)
		if (g_str_has_suffix(name, ".blob"))
			n++;
	return n;
}

static void
test_temporary_blobs(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GsurfView) t1 = NULL;
	g_autoptr(GsurfView) t2 = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *cwd = g_get_current_dir();
	g_autofree gchar *dir1 = NULL;
	g_autofree gchar *dir2 = NULL;
	g_autofree gchar *runtime = NULL;
	g_autofree gchar *id1 = NULL;
	g_autofree gchar *id2 = NULL;
	GsurfContainer *c1;
	GsurfContainer *c2;
	GStatBuf st;
	guint16 port;
	guint round;
	guint i;

	if (!gsurf_webkit_test_init())
		return;

	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(server);
	uri = g_strdup_printf("http://127.0.0.1:%u/", port);

	/* Two temporary containers live at the same time. */
	t1 = gsurf_view_new_full(NULL, GSURF_CONTAINER_TEMPORARY, NULL, FALSE);
	t2 = gsurf_view_new_full(NULL, GSURF_CONTAINER_TEMPORARY, NULL, FALSE);
	c1 = gsurf_container_registry_lookup(registry, gsurf_view_get_container_id(t1));
	c2 = gsurf_container_registry_lookup(registry, gsurf_view_get_container_id(t2));
	g_assert_nonnull(c1);
	g_assert_nonnull(c2);
	g_assert_true(c1 != c2);
	id1 = g_strdup(gsurf_container_get_id(c1));
	id2 = g_strdup(gsurf_container_get_id(c2));
	gsurf_webkit_test_load(gsurf_webkit_test_native(t1), uri);
	gsurf_webkit_test_load(gsurf_webkit_test_native(t2), uri);

	/* Several databases per container, interleaved, twice over: with
	 * the old ephemeral sessions the second blob anywhere hit EEXIST. */
	for (round = 0; round < 2; round++) {
		for (i = 0; i < 3; i++) {
			g_autofree gchar *call1 = g_strdup_printf("put('db%u','k%u','one-%u-%u')",
				i, round, round, i);
			g_autofree gchar *call2 = g_strdup_printf("put('db%u','k%u','two-%u-%u')",
				i, round, round, i);
			g_autofree gchar *want1 = g_strdup_printf("ok:one-%u-%u", round, i);
			g_autofree gchar *want2 = g_strdup_printf("ok:two-%u-%u", round, i);
			g_autofree gchar *got1 = settle(t1, call1);
			g_autofree gchar *got2 = settle(t2, call2);

			g_assert_cmpstr(got1, ==, want1);
			g_assert_cmpstr(got2, ==, want2);
		}
	}

	/* Isolation: a database t1 filled is empty in a third temporary
	 * container (t2 filled its own db0, so check a name only t1 used). */
	{
		g_autofree gchar *got = settle(t1, "put('only-one','k','x')");
		g_autofree gchar *seen = settle(t2, "count('only-one')");

		g_assert_cmpstr(got, ==, "ok:x");
		g_assert_cmpstr(seen, ==, "ok:0");
	}

	/* Nothing was written into the working directory. */
	g_assert_cmpuint(count_blobs(cwd), ==, 0);

	/* The data lives in owner-only directories under the runtime dir. */
	runtime = g_build_filename(g_get_user_runtime_dir(), "gsurf", NULL);
	dir1 = g_strdup(gsurf_container_get_data_dir(c1));
	dir2 = g_strdup(gsurf_container_get_data_dir(c2));
	g_assert_nonnull(dir1);
	g_assert_nonnull(dir2);
	g_assert_cmpstr(dir1, !=, dir2);
	g_assert_true(g_str_has_prefix(dir1, runtime));
	g_assert_true(g_str_has_prefix(dir2, runtime));
	g_assert_cmpint(g_stat(dir1, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);

	/* Closing the views and letting the cleanup run removes the data. */
	gsurf_container_registry_set_temp_cleanup_delay(registry, 0);
	g_clear_object(&t1);
	g_clear_object(&t2);
	for (i = 0; i < 200 && (g_file_test(dir1, G_FILE_TEST_EXISTS) ||
	                        g_file_test(dir2, G_FILE_TEST_EXISTS)); i++)
		gsurf_webkit_test_spin(25);
	g_assert_false(g_file_test(dir1, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(dir2, G_FILE_TEST_EXISTS));
	/* The network process does not bring them back afterwards. */
	gsurf_webkit_test_spin(500);
	g_assert_false(g_file_test(dir1, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(dir2, G_FILE_TEST_EXISTS));
	g_assert_null(gsurf_container_registry_lookup(registry, id1));
	g_assert_null(gsurf_container_registry_lookup(registry, id2));
	g_assert_cmpuint(count_blobs(cwd), ==, 0);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *home = g_dir_make_tmp("gsurf-temp-storage-gui-XXXXXX", NULL);
	g_autofree gchar *data = g_build_filename(home, "data", NULL);
	g_autofree gchar *cache = g_build_filename(home, "cache", NULL);
	g_autofree gchar *runtime = g_build_filename(home, "runtime", NULL);
	g_autofree gchar *work = g_build_filename(home, "cwd", NULL);
	g_autofree gchar *origin = g_get_current_dir();
	gint result;

	/* Private XDG dirs, and an empty working directory to watch. The
	 * Wayland socket is found through an absolute WAYLAND_DISPLAY or the
	 * real runtime dir, so only this process's view of it changes. */
	g_mkdir_with_parents(runtime, 0700);
	g_mkdir_with_parents(work, 0700);
	if (g_getenv("WAYLAND_DISPLAY") != NULL && !g_path_is_absolute(g_getenv("WAYLAND_DISPLAY"))
	    && g_getenv("XDG_RUNTIME_DIR") != NULL) {
		g_autofree gchar *socket = g_build_filename(g_getenv("XDG_RUNTIME_DIR"),
			g_getenv("WAYLAND_DISPLAY"), NULL);

		g_setenv("WAYLAND_DISPLAY", socket, TRUE);
	}
	g_setenv("XDG_DATA_HOME", data, TRUE);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	g_assert_cmpint(g_chdir(work), ==, 0);

	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/temp-storage/webkit/indexeddb-blobs", test_temporary_blobs);
	result = g_test_run();
	g_chdir(origin);
	gsurf_test_rmtree(home);
	return result;
}
