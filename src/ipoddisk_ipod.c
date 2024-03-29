/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#include "ipoddisk.h"

#define IPODDISK_MAX_IPOD       16
static int ipodnr;
static struct ipoddisk_node *ipods[IPODDISK_MAX_IPOD];
static struct ipoddisk_node *ipoddisk_tree;
static GError *error = NULL;


int
ipoddisk_statipods (struct statvfs *stbuf)
{
        struct statvfs tmp;
        int i;

        if (statvfs(ipods[0]->nd_data.ipod.ipod_mp, stbuf) == -1)
                return -errno;

        for (i = 1; i < ipodnr; i++) {
                if (statvfs(ipods[i]->nd_data.ipod.ipod_mp, &tmp) == -1)
                        return -errno;
                /* FIXME: this assumes that block sizes of all filesystems
                 * are the same */
                stbuf->f_blocks += tmp.f_blocks;
                stbuf->f_bavail += tmp.f_bavail;
                stbuf->f_bfree += tmp.f_bfree;
        }

        stbuf->f_flag = ST_RDONLY | ST_NOSUID;

        return 0;
}

#define IPODDISK_MAX_PATH_TOKENS	12

struct ipoddisk_node *
ipoddisk_parse_path (const char *path, int len)
{
	struct ipoddisk_node *node;
	struct ipoddisk_node *parent;
	gchar                **orig;
	gchar                **tokens;

	UNUSED(len);

        node   =
	parent = ipoddisk_tree;
	orig   =
        tokens = g_strsplit(path, "/", IPODDISK_MAX_PATH_TOKENS);

	while (*tokens) {
		gchar *token = *tokens;

		tokens++;
                if (strlen(token) == 0)
                        continue;

                if (parent->nd_type == IPODDISK_NODE_LEAF) {
                        node = NULL;
                        break;
                }

                node = g_datalist_get_data(&parent->nd_children, token);
                if (node == NULL)
                        break;
                parent = node;
	}

	g_strfreev(orig);
	return node;
}

#undef IPODDISK_MAX_PATH_TOKENS

/* FIXME: assume track->ipod_path has an extension 
   of 4 char, e.g. '.mp3', '.m4a'. */
#define IPOD_TRACK_EXTENSION_LEN	4

static inline gchar *
ipod_get_track_extension (gchar * path)
{
	gchar *ext;

        assert (strlen(path) > IPOD_TRACK_EXTENSION_LEN);

	ext = path + strlen(path) - IPOD_TRACK_EXTENSION_LEN;
	return ext;
}

static void
ipoddisk_encode_name (gchar **strpp)
{
	gchar *old = *strpp;
        gchar *nstr;         /* normalized utf-8 string */
	int    i;

        if (old == NULL)
                return;

	/* Encode path names to appease Finder:
	 * 0. leading . is treated as hidden file, encode as _
	 * 1. slash is Unix path separator, encode as : 
	 * 2. \r and \n are problematic, encode as space
	 * Then normalize the string to cope with tricky
	 * things like umlaut */
        for (i = 0; i < strlen(old); i++) {
                if (i == 0 && old[i] == '.')
                        old[i] = '_';
                else if (old[i] == '/')
                        old[i] = ':';
                else if (old[i] == '\r' || old[i] == '\n')
                        old[i] = ' ';
        }
        
        nstr = g_utf8_normalize(old, -1, G_NORMALIZE_NFD);
        if (nstr) {
                g_free(old);
                *strpp = nstr;
        }

        return;
}

/**
 * Adds a child to a parent node, and enure uniqueness of its key
 */
static void
ipoddisk_add_child (struct ipoddisk_node *parent,
                    struct ipoddisk_node *child, gchar *key)
{
        int    ndup = 0;
        gchar *new_key = g_strdup(key);
        
        while (g_datalist_get_data(&parent->nd_children, new_key)) {
                ndup++;
                g_free(new_key);
                new_key = g_strdup_printf("(%d) %s", ndup, key);
        }

        g_datalist_set_data(&parent->nd_children, new_key, child);
        g_free(new_key);

        return;
}

static inline struct ipoddisk_node *
ipoddisk_new_node (struct ipoddisk_node *parent, gchar *key,
                   ipoddisk_node_type type)
{
        struct ipoddisk_node *node = g_slice_new(struct ipoddisk_node);

        assert (parent == NULL || key != NULL);

        node->nd_type = type;

        if (type != IPODDISK_NODE_LEAF) /* leaf node has no children */
                g_datalist_init(&node->nd_children);

        if (parent != NULL)
                ipoddisk_add_child(parent, node, key);

        return node;
}

/**
 * Adds a track into a tree structure
 * @param itdbtrk Pointer to the track's Itdb_Track structure
 * @param start Pointer to the root of the tree
 * @param track If not NULL, pointer to the node of the track
 * @param ipod Root node of this track
 */
static void
ipoddisk_add_track (Itdb_Track *itdbtrk,
                    struct ipoddisk_node *start, struct ipoddisk_node *albums,
                    struct ipoddisk_node *track, struct ipoddisk_ipod *ipod)
{
	struct ipoddisk_node *artist;
	struct ipoddisk_node *album;
	gchar                *track_ext;
	gchar                *album_name;
	gchar                *track_name;
	gchar                *artist_name;

	album_name  = itdbtrk->album ? itdbtrk->album : "Unknown Album";
	track_name  = itdbtrk->title ? itdbtrk->title : "Unknown Track";
	artist_name = itdbtrk->artist ? itdbtrk->artist : "Unknown Artist";

	artist = g_datalist_get_data(&start->nd_children, artist_name);
	if (!artist)
		artist = ipoddisk_new_node(start, artist_name,
                                           IPODDISK_NODE_DEFAULT);

	album = g_datalist_get_data(&artist->nd_children, album_name);
	if (!album) {
		album = ipoddisk_new_node(artist, album_name,
                                          IPODDISK_NODE_DEFAULT);
                if (albums != NULL)
                        ipoddisk_add_child(albums, album, album_name);
        }

        track_ext = ipod_get_track_extension(itdbtrk->ipod_path);
        track_name = g_strconcat(track_name, track_ext, NULL);

        if (track != NULL) {
                ipoddisk_add_child(album, track, track_name);
        } else {
                track = ipoddisk_new_node(album, track_name,
                                          IPODDISK_NODE_LEAF);
                track->nd_children = (GData *) itdbtrk;
                track->nd_data.track.trk_ipod = ipod;
                itdbtrk->userdata = track;
        }

        g_free(track_name);
	return;
}

struct __add_member_arg {
        char                 *prefixfmt;
        int                   counter;
        struct ipoddisk_node *ipod;
        struct ipoddisk_node *playlist;
};

static void
ipoddisk_add_playlist_member (gpointer data, gpointer user_data)
{
	Itdb_Track              *itdbtrk = data;
	struct __add_member_arg *argp = user_data;
	struct ipoddisk_node    *track = itdbtrk->userdata;
	gchar                   *track_ext;
	gchar                   *track_name;

	track_ext = ipod_get_track_extension(itdbtrk->ipod_path);
	track_name = itdbtrk->title ? itdbtrk->title : "Unknown Track";
        
        if (argp->prefixfmt) {
                gchar *prefix;

                prefix = g_strdup_printf(argp->prefixfmt, argp->counter);
                track_name = g_strconcat(prefix, track_name, track_ext, NULL);
                
                g_free(prefix);
        } else {
                track_name = g_strconcat(track_name, track_ext, NULL);
        }

        if (track == NULL) {
                track = ipoddisk_new_node(argp->playlist, track_name,
                                          IPODDISK_NODE_LEAF);
                /* TODO: store itdbtrk in rack->nd_data.track */
                track->nd_children = (GData *) itdbtrk;
                track->nd_data.track.trk_ipod = &argp->ipod->nd_data.ipod;
                itdbtrk->userdata = track;
        } else {
                ipoddisk_add_child(argp->playlist, track, track_name);
        }

	argp->counter++;
        g_free(track_name);
	return;
}

void
ipoddisk_build_ipod_node (struct ipoddisk_node *root, Itdb_iTunesDB *itdb)
{
        GList                *list;
        struct ipoddisk_node *genres;
        struct ipoddisk_node *albums;
        struct ipoddisk_node *artists;
        struct ipoddisk_node *compilations;
        struct ipoddisk_node *playlists;

        genres       = ipoddisk_new_node(root, "Genres", IPODDISK_NODE_DEFAULT);
        albums       = ipoddisk_new_node(root, "Albums", IPODDISK_NODE_DEFAULT);
        artists      = ipoddisk_new_node(root, "Artists", IPODDISK_NODE_DEFAULT);
        playlists    = ipoddisk_new_node(root, "Playlists", IPODDISK_NODE_DEFAULT);
        compilations = ipoddisk_new_node(root, "Compilations", IPODDISK_NODE_DEFAULT);

        /* Populate iPodDisk/(Artists|Albums|Genres) */
        list = itdb->tracks;
        while (list) {
                Itdb_Track           *itdbtrk = list->data;
                struct ipoddisk_node *comp;
                gchar                *comp_title;

                list = g_list_next(list);

                ipoddisk_encode_name(&itdbtrk->album);
                ipoddisk_encode_name(&itdbtrk->title);
                ipoddisk_encode_name(&itdbtrk->genre);
                ipoddisk_encode_name(&itdbtrk->artist);

                ipoddisk_add_track(itdbtrk, artists, albums,
                                   NULL, &root->nd_data.ipod);

                if (itdbtrk->genre != NULL && strlen(itdbtrk->genre) != 0) {
                        struct ipoddisk_node *genre;
                        
                        genre = g_datalist_get_data(&genres->nd_children, itdbtrk->genre);
                        if (genre == NULL)
                                genre = ipoddisk_new_node(genres, itdbtrk->genre,
                                                          IPODDISK_NODE_DEFAULT);
                        ipoddisk_add_track(itdbtrk, genre, NULL,
                                           (struct ipoddisk_node *) itdbtrk->userdata, NULL);
                }

                if (!itdbtrk->compilation || /* not part of a compilation */
                    itdbtrk->album == NULL || itdbtrk->title == NULL)
                        continue;

                comp = g_datalist_get_data(&compilations->nd_children, itdbtrk->album);
                if (comp == NULL)
                        comp = ipoddisk_new_node(compilations, itdbtrk->album,
                                                 IPODDISK_NODE_DEFAULT);

                comp_title = g_strconcat(itdbtrk->title,
                                         ipod_get_track_extension(itdbtrk->ipod_path), 
                                         NULL);
                assert (itdbtrk->userdata != NULL);
                ipoddisk_add_child(comp, itdbtrk->userdata, comp_title);
                g_free(comp_title);
        }

        /* Populate iPodDisk/Playlists */
        list = itdb->playlists;
        while (list) {
                Itdb_Playlist          *itdbpl = list->data;
                gchar                  *pl_name;
                guint                   cnt;
                struct ipoddisk_node   *pl;
                struct __add_member_arg arg;

                list = g_list_next(list);

                if (itdb_playlist_is_mpl(itdbpl)) {
                        pl_name = "Master Playlist";
                        continue; /* ignore mpl for now, make it optional in the future */
                } else {
                        ipoddisk_encode_name(&itdbpl->name);
                        pl_name = itdbpl->name ? itdbpl->name : "Unknown Playlist";
                }

                pl = g_datalist_get_data(&playlists->nd_children, pl_name);
                if (pl == NULL)
                        pl = ipoddisk_new_node(playlists, pl_name,
                                               IPODDISK_NODE_DEFAULT);

                cnt = g_list_length(itdbpl->members);
                if (cnt == 1) {
                        arg.prefixfmt = NULL;
                } else if (cnt < 10) {
                        arg.prefixfmt = "%d. ";
                } else if (cnt < 100) {
                        arg.prefixfmt = "%.2d. ";
                } else if (cnt < 1000) {
                        arg.prefixfmt = "%.3d. ";
                } else {
                        arg.prefixfmt = "%.4d. ";
                }

                arg.counter  = 1;
                arg.playlist = pl;
                arg.ipod     = root;

		g_list_foreach(itdbpl->members,
                               ipoddisk_add_playlist_member, &arg);
        }

        return;
}

gchar *
ipoddisk_node_path (struct ipoddisk_node *node)
{
        gchar      *rpath; /* relative path */
        gchar      *apath; /* absolute path */
        Itdb_Track *track;

        assert(node->nd_type == IPODDISK_NODE_LEAF);

        track = (Itdb_Track *) node->nd_children;
        rpath = g_strdup(track->ipod_path);
        itdb_filename_ipod2fs(rpath);

        assert (*rpath == '/');

	apath = g_strconcat(node->nd_data.track.trk_ipod->ipod_mp, rpath, NULL);

        g_free(rpath);
        return apath;
}

struct ipoddisk_node *
ipoddisk_init_one_ipod (gchar *dbfile)
{
        struct ipoddisk_node *node;
        Itdb_iTunesDB        *the_itdb;

        the_itdb = itdb_parse_file(dbfile, &error);
	if (error != NULL) {
                fprintf(stderr,
                        "itdb_parse_file() failed: %s!\n",
                        error->message);
		g_error_free(error);
		error = NULL;
                return NULL;
	}

	if (the_itdb == NULL)
		return NULL;

        node = ipoddisk_new_node(NULL, NULL, IPODDISK_NODE_IPOD);
        node->nd_data.ipod.ipod_itdb = the_itdb;

        ipoddisk_build_ipod_node(node, the_itdb);

        open(dbfile, O_RDONLY); /* leave me not, babe */

	return node;
}

int
ipoddisk_init_ipods (void)
{
	int                  i;
        int                  fsnr;
	struct statfs        *stats = NULL;

	fsnr = getfsstat(NULL, 0, MNT_NOWAIT);
	if (fsnr <= 0)
		return ENOENT;

	stats = g_malloc0(fsnr * sizeof(struct statfs));
        if (stats == NULL)
                return ENOMEM;

	fsnr = getfsstat(stats, fsnr * sizeof(struct statfs), MNT_NOWAIT);
	if (fsnr <= 0) {
		g_free(stats);
		return ENOENT;
        }

        ipoddisk_tree = ipoddisk_new_node(NULL, NULL, IPODDISK_NODE_ROOT);

        for (i = 0, ipodnr = 0; i < fsnr && ipodnr < IPODDISK_MAX_IPOD; i++) {
                gchar                *dbpath;
                gchar                *ipodname;
                struct ipoddisk_node *node;

                if (strncasecmp(stats[i].f_mntfromname,
                                CONST_STR_LEN("/dev/disk")))
                        continue;  /* fs not disk-based */

                if (!strcmp(stats[i].f_mntonname, "/"))
                        continue;  /* skip root fs */

                dbpath = g_strconcat(stats[i].f_mntonname,
                                     "/iPod_Control/iTunes/iTunesDB", NULL);

                if (strlen(dbpath) >= MAXPATHLEN ||
                    !g_file_test(dbpath, G_FILE_TEST_EXISTS) ||
                    !g_file_test(dbpath, G_FILE_TEST_IS_REGULAR)) {
                        g_free(dbpath);
                        continue;
                }

                node = ipoddisk_init_one_ipod(dbpath);
                if (node == NULL) {
                        g_free(dbpath);
                        continue;
                }

                ipodname = g_path_get_basename(stats[i].f_mntonname);
                ipoddisk_add_child(ipoddisk_tree, node, ipodname);
                node->nd_data.ipod.ipod_mp = g_strdup(stats[i].f_mntonname);

                ipods[ipodnr] = node;
                ipodnr++;
                
                g_free(dbpath);
                g_free(ipodname);
        }

        g_free(stats);

        if (ipodnr == 0)
                return ENOENT;

        if (ipodnr == 1) {
                ipoddisk_tree = ipods[0];
                ipoddisk_tree->nd_type = IPODDISK_NODE_ROOT;
        }

        return 0;
}

void
ipod_free(void)
{
	/* no need to clean up:
           if (the_itdb)
                   itdb_free(the_itdb);
	   if (artists)
	   g_datalist_clear(&artists);
	   if (playlists)
	   g_datalist_clear(&playlists);
	 */

	/* TODO: clear datalists within artists and playlists */

	return;
}
