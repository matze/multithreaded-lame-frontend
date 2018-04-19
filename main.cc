#include <iostream>
#include <algorithm>
#include <fstream>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <cstring>
#include <condition_variable>
#include <experimental/filesystem>
#include <lame/lame.h>

namespace fs = std::experimental::filesystem;

const int FRAME_SIZE = 1152;

typedef struct {
    char id[4];
    uint32_t size;
} Chunk;

typedef struct {
    struct {
        Chunk chunk;
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
} WaveHeader;

template <typename T>
class AsyncQueue {
public:
    T pop() {
        std::unique_lock<std::mutex> mlock(mutex);

        while (queue.empty())
            cond.wait(mlock);

        auto item = queue.front();
        queue.pop();
        return item;
    }

    void push(const T& item) {
        std::unique_lock<std::mutex> mlock(mutex);
        queue.push(item);
        mlock.unlock();
        cond.notify_one();
    }

private:
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable cond;
};

typedef enum {
    Job,
    Terminate,
} MessageType;

struct Message {
    MessageType type;
    fs::path path;
};

static void
encode_mp3 (fs::path wav_path)
{
    WaveHeader header;
    Chunk chunk = {0,};

    std::ifstream input (wav_path, std::ios::binary);

    input.read ((char *) &header, sizeof (header));

    if (strncmp (header.riff.chunk.id, "RIFF", 4) || 
        strncmp (header.riff.type, "WAVE", 4) ||
        strncmp (header.fmt.signature, "fmt ", 4)) {
        std::cerr << wav_path << " has wrong data format\n";
        return;
    }

    input.read ((char *) &chunk, sizeof (chunk));

    /* skip non-data chunks */
    while (strncmp (chunk.id, "data", 4)) {
        input.seekg (chunk.size, input.cur);
        input.read ((char *) &chunk, sizeof (chunk));
    }

    bool is_mono = header.fmt.num_channels == 1;
    int num_samples = chunk.size / header.fmt.frame_size;
    size_t out_length = header.fmt.num_channels * FRAME_SIZE * 2;
    unsigned char *out_data = new unsigned char[out_length];
    short int *in_data = new short int[chunk.size];

    input.read ((char *) in_data, chunk.size);

    lame_t lame = lame_init ();
    lame_set_num_channels (lame, header.fmt.num_channels);
    lame_set_in_samplerate (lame, header.fmt.sample_rate);
    lame_set_out_samplerate (lame, header.fmt.sample_rate);
    lame_set_mode (lame, is_mono ? MONO : STEREO);
    lame_set_quality (lame, 3);

    if (lame_init_params (lame) < 0) {
        std::cerr << "Could not initialize LAME\n";
        return;
    }

    std::ofstream output (wav_path.replace_extension (".mp3"), std::ios::binary);

    for (int current = 0; current < num_samples;) {
        size_t to_read = std::min (FRAME_SIZE, num_samples - current);
        size_t encoded;

        if (is_mono)
            encoded = lame_encode_buffer (lame, &in_data[current], NULL, to_read, out_data, out_length);
        else
            encoded = lame_encode_buffer_interleaved (lame, &in_data[current], to_read, out_data, out_length);

        if (encoded > 0)
            output.write ((char *) out_data, encoded);

        current += is_mono ? FRAME_SIZE : FRAME_SIZE * 2;
    }

    size_t encoded = lame_encode_flush (lame, out_data, out_length);

    if (encoded > 0)
        output.write ((char *) out_data, encoded);
    
    lame_close (lame);

    delete[] out_data;
    delete[] in_data;
}

static void
worker(AsyncQueue<Message> &queue) 
{
    while (1) {
        auto message = queue.pop();

        if (message.type == MessageType::Terminate)
            break;

        encode_mp3 (message.path);
    }
}

class ThreadPool {
public:
    ThreadPool(unsigned size) {
        for (unsigned i = 0; i < size; i++)
            threads.push_back (std::thread (worker, std::ref (queue)));
    };

    virtual ~ThreadPool () {
        for (unsigned i = 0; i < threads.size(); i++)
            queue.push (Message { Terminate });

        for (auto &thread: threads)
            thread.join ();
    }

    void submit(const fs::path path) {
        queue.push (Message { Job, path });
    }

private:
    AsyncQueue<Message> queue;
    std::vector<std::thread> threads;
};

auto
get_valid_filenames(fs::path root)
{
    std::vector<fs::path> result;

    if (fs::exists (root)) {
        for (auto &entry: fs::directory_iterator (root)) {
            if (entry.path().extension() == ".wav")
                result.push_back (entry);
        }
    }

    return result;
}

static void
usage (const char *prog)
{
    std::cout << "Usage: " << prog << " <dir-to-wavs>\n";
    exit (0);
}

int
main (int argc, char const* argv[])
{
    if (argc < 2)
        usage (argv[0]);

    auto filenames = get_valid_filenames (fs::path (argv[1]));

    if (filenames.size () == 0) {
        std::cout << "No .wav files found\n";
        usage (argv[0]);
    }

    ThreadPool pool(std::thread::hardware_concurrency ());

    for (auto filename: filenames)
        pool.submit (filename);

    return 0;
}
