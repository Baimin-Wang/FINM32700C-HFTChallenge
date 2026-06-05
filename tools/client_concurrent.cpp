#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <cctype>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

static constexpr int MODULO = 997;

struct Challenge {
    int id;
    int N;
    vector<int> data; // flat: A[N*N] then B[N*N]
};

static mutex qMutex;
static condition_variable qCV;
static queue<Challenge> workQueue;
static atomic<bool> shuttingDown{false};

static mutex sendMutex;

static int computeChecksum(const vector<int>& data, int N) {
    vector<int> colSumA(N, 0);
    vector<int> rowSumB(N, 0);
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            colSumA[j] = (colSumA[j] + data[i * N + j]) % MODULO;
            rowSumB[i] = (rowSumB[i] + data[N * N + i * N + j]) % MODULO;
        }
    int checksum = 0;
    for (int k = 0; k < N; k++)
        checksum = (checksum + colSumA[k] * rowSumB[k]) % MODULO;
    return checksum;
}

static bool sendAll(int sock, const string& msg) {
    const char* p = msg.data();
    size_t left = msg.size();
    while (left > 0) {
        ssize_t n = send(sock, p, left, 0);
        if (n <= 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

static void workerThread(int sock) {
    while (true) {
        Challenge ch;
        {
            unique_lock<mutex> lock(qMutex);
            qCV.wait(lock, [] { return !workQueue.empty() || shuttingDown.load(); });
            if (workQueue.empty()) return;
            ch = std::move(workQueue.front());
            workQueue.pop();
        }

        int answer = computeChecksum(ch.data, ch.N);
        string reply = to_string(ch.id) + " " + to_string(answer) + "\n";

        {
            lock_guard<mutex> lock(sendMutex);
            sendAll(sock, reply);
        }

        cout << "Challenge " << ch.id << " -> " << answer << "\n";
    }
}

static void parseInts(const char* buf, int n, vector<int>& nums, int& cur, bool& reading) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (isdigit(c)) {
            cur = cur * 10 + (c - '0');
            reading = true;
        } else if (reading) {
            nums.push_back(cur);
            cur = 0;
            reading = false;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <host> <port> <team>\n";
        return 1;
    }

    string host = argv[1];
    int port = stoi(argv[2]);
    string team = argv[3];

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    string intro = team + "\n";
    send(sock, intro.c_str(), intro.size(), 0);
    cout << "Connected to " << host << ":" << port << " as " << team << "\n";

    int nThreads = (int)max(2u, thread::hardware_concurrency());
    cout << "Using " << nThreads << " worker threads\n";

    vector<thread> workers;
    for (int i = 0; i < nThreads; i++)
        workers.emplace_back(workerThread, sock);

    char buffer[65536];
    vector<int> nums;
    size_t numStart = 0;
    int curVal = 0;
    bool reading = false;

    while (true) {
        int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0) { cout << "Disconnected.\n"; break; }

        parseInts(buffer, n, nums, curVal, reading);

        while (nums.size() - numStart >= 2) {
            int id = nums[numStart];
            int N = nums[numStart + 1];
            if (N <= 0) { cerr << "Invalid N=" << N << "\n"; goto cleanup; }

            size_t needed = 2 + 2 * (size_t)N * N;
            if (nums.size() - numStart < needed) break;

            {
                Challenge ch;
                ch.id = id;
                ch.N = N;
                ch.data.assign(nums.begin() + numStart + 2,
                               nums.begin() + numStart + needed);
                lock_guard<mutex> lock(qMutex);
                workQueue.push(std::move(ch));
                qCV.notify_one();
            }

            numStart += needed;
            if (numStart > 100000) {
                nums.erase(nums.begin(), nums.begin() + numStart);
                numStart = 0;
            }
        }
    }

cleanup:
    shuttingDown = true;
    qCV.notify_all();
    for (auto& w : workers) w.join();
    close(sock);
    return 0;
}
