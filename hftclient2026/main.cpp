#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <cctype>
#include <chrono>
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

static int computeChecksumFast(const vector<int>& data, int N) {
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

static bool sendAll(int sock, const char* p, size_t left) {
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

        auto computeStart = chrono::high_resolution_clock::now();
        int answer = computeChecksumFast(ch.data, ch.N);
        auto computeUs = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now() - computeStart).count();

        char reply[32];
        int replyLen = snprintf(reply, sizeof(reply), "%d %d\n", ch.id, answer);

        auto sendStart = chrono::high_resolution_clock::now();
        {
            lock_guard<mutex> lock(sendMutex);
            sendAll(sock, reply, (size_t)replyLen);
        }
        auto sendUs = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now() - sendStart).count();

        cout << "Answered challenge " << ch.id << " with " << answer
             << " (compute " << computeUs << " us, send " << sendUs << " us)\n";
    }
}

static void parseIncomingInts(const char* buffer, int n, vector<int>& nums,
                               int& currentValue, bool& readingNumber) {
    for (int i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)buffer[i];
        if (isdigit(ch)) {
            currentValue = currentValue * 10 + (ch - '0');
            readingNumber = true;
        } else if (readingNumber) {
            nums.push_back(currentValue);
            currentValue = 0;
            readingNumber = false;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        cout << "Usage: " << argv[0] << " <host> <port> <team_name>\n";
        return 1;
    }

    string host = argv[1];
    int port    = stoi(argv[2]);
    string team = argv[3];

    cout << "HFT Client\n";
    cout << "Solving matrix checksum challenges.\n\n";

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    string intro = team + "\n";
    send(sock, intro.c_str(), intro.size(), 0);

    int nThreads = (int)max(2u, thread::hardware_concurrency());
    cout << "Connected to " << host << ":" << port
         << " | threads=" << nThreads << "\n";
    cout << "Waiting for challenges...\n";

    vector<thread> workers;
    for (int i = 0; i < nThreads; i++)
        workers.emplace_back(workerThread, sock);

    char buffer[65536];
    vector<int> nums;
    size_t numStart    = 0;
    int currentValue   = 0;
    bool readingNumber = false;

    while (true) {
        int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            cout << "Disconnected from server.\n";
            break;
        }

        parseIncomingInts(buffer, n, nums, currentValue, readingNumber);

        while (nums.size() - numStart >= 2) {
            int challengeId = nums[numStart];
            int N           = nums[numStart + 1];

            if (N <= 0) {
                cerr << "Invalid matrix size: " << N << "\n";
                goto cleanup;
            }

            size_t valuesNeeded = 2 + 2 * (size_t)N * N;
            if (nums.size() - numStart < valuesNeeded) break;

            {
                Challenge ch;
                ch.id = challengeId;
                ch.N  = N;
                ch.data.assign(nums.begin() + numStart + 2,
                               nums.begin() + numStart + valuesNeeded);
                lock_guard<mutex> lock(qMutex);
                workQueue.push(std::move(ch));
                qCV.notify_one();
            }

            numStart += valuesNeeded;
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
