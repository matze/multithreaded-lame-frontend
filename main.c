#ifdef _MSC_VER
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
#define HAVE_STRUCT_TIMESPEC    1
#include <Windows.h>
#else
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/stat.h>
#include <pthread.h>
#include <lame/lame.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

const int FRAME_SIZE = 1152;

typedef struct _List {
    void         *data;
    struct _List *next;
} List;

typedef void* (*ThreadFunc)(void *);

typedef struct {
    List            *threads;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    unsigned         max_threads;
    unsigned         num_busy;
} ThreadPool;

typedef struct {
    ThreadFunc   func;
    ThreadPool  *pool;
    void        *data;
} ThreadData;

typedef struct {
    struct {
        char chunk_id[4];
        uint32_t chunk_size;
        char type[4];
    } riff;
    struct {
        char signature[4];
        uint32_t length;
        uint16_t tag;
        uint16_t num_channels;
        uint32_t sample_rate;
        uint32_t frame_rate;
        uint16_t frame_size;
        uint16_t bits_per_sample;
    } fmt;
    struct {
        char signature[4];
        uint32_t length;
    } data;
} WaveHeader;

typedef struct {
    char *filename;
    char *message;
} Error;


static void
usage (const char *prog)
{
    printf ("Usage: %s <dir-to-wavs>\n", prog);
    exit (0);
}

static int
is_potential_wav_file (const char *path)
{
    struct stat info;

    if (stat (path, &info) < 0) {
        fprintf (stderr, "Error reading %s: %s\n", path, strerror (errno));
        return 0;
    }

    if (!(info.st_mode & S_IFREG) || (strlen (path) < 4))
        return 0;

    return strcmp (path + strlen (path) - 4, ".wav") == 0 ? 1 : 0;
}

static char *
path_join (const char *prefix, const char *basename)
{
    size_t length;
    char *path;

    length = strlen (prefix) + 1 + strlen (basename) + 1;
    path = malloc (length);
#ifndef _MSC_VER
    snprintf (path, length, "%s/%s", prefix, basename);
#else
    snprintf (path, length, "%s\\%s", prefix, basename);
#endif
    return path;
}

static List *
list_prepend (List *list, void *data)
{
    List *elem = malloc (sizeof (List));
    elem->data = data;
    elem->next = NULL;

    if (list != NULL)
        elem->next = list;
    else
        list = elem;

    return elem;
}

static void
list_free_full (List *list)
{
    for (List *it = list; it != NULL;) {
        List *tmp = it;
        free (it->data);
        it = it->next;
        free (tmp);
    }
}

static List *
get_valid_file_names (const char* root)
{
    List *list = NULL;

#ifndef _MSC_VER
    DIR *dir;
    struct dirent *entry;

    dir = opendir (root);

    if (dir) {
        while ((entry = readdir (dir)) != NULL) {
            char *path = path_join (root, entry->d_name);

            if (is_potential_wav_file (path))
                list = list_prepend (list, path);
            else
                free (path);
        }
    }

    closedir (dir);
#else
    char *search_path;
    HANDLE entry;
    WIN32_FIND_DATA info;

    search_path = path_join (root, "*");
    entry = FindFirstFile (TEXT (search_path), &info);

    if (entry != INVALID_HANDLE_VALUE) {
        do {
            char *path = path_join (root, info.cFileName);

            if (is_potential_wav_file (path))
                list = list_prepend (list, path);
            else
                free (path);
        } while (FindNextFile (entry, &info) != 0);
    }

    free(search_path);
#endif
    return list;
}

static unsigned
get_num_cores (void)
{
#ifdef _MSC_VER
    SYSTEM_INFO sysinfo;
    GetSystemInfo (&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#else
    return sysconf (_SC_NPROCESSORS_ONLN);
#endif
}

static Error *
error_new (const char *filename, const char *message)
{
    Error *error;

    error = malloc (sizeof (Error));
    error->filename = strdup (filename);
    error->message = strdup (message);
    return error;
}

static void *
encode_mp3 (const char *wav_path)
{
    FILE *mp3;
    FILE *wav;
    char *mp3_path;
    lame_t lame;
    WaveHeader header;
    size_t read;
    int current;
    int num_samples;
    int encoded;
    int out_length;
    int is_mono;
    unsigned char *out_data = NULL;
    short int *in_data = NULL;
    Error *error = NULL;

    mp3_path = strdup (wav_path);
    strncpy (mp3_path + strlen (mp3_path) - 3, "mp3", 3);

    lame = lame_init ();
    wav = fopen (wav_path, "rb");
    mp3 = fopen (mp3_path, "wb");

    fread (&header, sizeof (WaveHeader), 1, wav);

    if (strncmp (header.riff.chunk_id, "RIFF", 4) || strncmp (header.riff.type, "WAVE", 4) ||
        strncmp (header.fmt.signature, "fmt ", 4) || strncmp (header.data.signature, "data", 4)) {
        error = error_new (wav_path, "Invalid data format");
        goto cleanup;
    }

    if (header.fmt.tag != 1) {
        error = error_new (wav_path, "Non-PCM data not supported");
        goto cleanup;
    }

    if (header.fmt.bits_per_sample != 16) {
        error = error_new (wav_path, "PCM data other than 16 bits not supported");
        goto cleanup;
    }

    out_length = header.fmt.num_channels * FRAME_SIZE * 2;
    out_data = malloc (out_length);
    in_data = malloc (header.data.length);
    read = fread (in_data, 1, header.data.length, wav);
    is_mono = header.fmt.num_channels == 1;

    if (header.data.length != read) {
        error = error_new (wav_path, "Not enough data to read");
        goto cleanup;
    }

    lame_set_num_channels (lame, header.fmt.num_channels);
    lame_set_in_samplerate (lame, header.fmt.sample_rate);
    lame_set_out_samplerate (lame, header.fmt.sample_rate);
    lame_set_mode (lame, is_mono == 1 ? MONO : STEREO);
    lame_set_quality (lame, 3);

    if (lame_init_params (lame) < 0) {
        error = error_new (wav_path, "LAME initialization failed");
        goto cleanup;
    }

    num_samples = header.data.length / header.fmt.frame_size;

    for (current = 0; current < num_samples;) {
        read = MIN (FRAME_SIZE, num_samples - current);

        if (is_mono)
            encoded = lame_encode_buffer (lame, &in_data[current], NULL, read, out_data, out_length);
        else
            encoded = lame_encode_buffer_interleaved (lame, &in_data[current], read, out_data, out_length);

        if (encoded > 0)
            fwrite (out_data, encoded, 1, mp3);

        current += is_mono ? FRAME_SIZE : FRAME_SIZE * 2;
    }

    encoded = lame_encode_flush (lame, out_data, out_length);

    if (encoded > 0)
        fwrite (out_data, encoded, 1, mp3);

cleanup:
    lame_close (lame);
    fclose (wav);
    fclose (mp3);
    free (in_data);
    free (out_data);
    free (mp3_path);

    return error;
}

static ThreadPool *
thread_pool_new (void)
{
    ThreadPool *pool;

    pool = malloc (sizeof (ThreadPool));
    pool->max_threads = get_num_cores ();
    pool->num_busy = 0;
    pool->threads = NULL;

    pthread_mutex_init (&pool->mutex, NULL);
    pthread_cond_init (&pool->cond, NULL);

    return pool;
}

static void*
thread_pool_runner (ThreadData *data)
{
    void *result;

    result = data->func (data->data);

    pthread_mutex_lock (&data->pool->mutex);
    data->pool->num_busy--;
    pthread_cond_signal (&data->pool->cond);
    pthread_mutex_unlock (&data->pool->mutex);

    /* we took ownership */
    free (data);

    return result;
}

static void
thread_pool_submit (ThreadPool *pool, ThreadFunc func, void *data)
{
    ThreadData *thread_data;
    pthread_t *thread;

    pthread_mutex_lock (&pool->mutex);

    while (pool->num_busy == pool->max_threads)
        pthread_cond_wait (&pool->cond, &pool->mutex);

    thread_data = malloc (sizeof (ThreadData));
    thread_data->pool = pool;
    thread_data->func = func;
    thread_data->data = data;

    thread = malloc (sizeof (pthread_t));
    pthread_create (thread, NULL, (ThreadFunc) thread_pool_runner, thread_data);
    pool->num_busy++;
    pool->threads = list_prepend (pool->threads, thread);

    pthread_mutex_unlock (&pool->mutex);
}

static void
thread_pool_wait (ThreadPool *pool)
{
    for (List *it = pool->threads; it != NULL; it = it->next) {
        void *result = NULL;

        pthread_join (*((pthread_t *) it->data), &result);

        if (result != NULL) {
            Error *error = (Error *) result;
            fprintf (stderr, "Error: %s: %s\n", error->filename, error->message);
        }
    }
}

static void
thread_pool_free (ThreadPool *pool)
{
    list_free_full (pool->threads);
    free (pool);
}

int
main (int argc, char const* argv[])
{
    List *filenames;
    ThreadPool *pool;

    if (argc < 2)
        usage (argv[0]);

    filenames = get_valid_file_names (argv[1]);

    if (filenames == NULL) {
        printf ("No .wav files found\n");
        usage (argv[0]);
    }

    pool = thread_pool_new ();

    for (List *it = filenames; it != NULL; it = it->next)
        thread_pool_submit (pool, (ThreadFunc) encode_mp3, it->data);

    thread_pool_wait (pool);
    thread_pool_free (pool);
    list_free_full (filenames);

    return 0;
}
