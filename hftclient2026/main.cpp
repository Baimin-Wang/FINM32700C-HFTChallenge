#include <iostream>
#include <string>
#include <vector>
#include <cctype>
#include <chrono>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

static constexpr int MODULO = 997;

int computeChecksumFast(const vector<int>& nums, int challengeStart, int N) {
    int matrixValues = N * N;
    int aStart = challengeStart + 2;
    int bStart = aStart + matrixValues;

    vector<int> colSumA(N, 0);
    vector<int> rowSumB(N, 0);

    for (int i = 0; i < N; ++i) {
        int rowBase = i * N;

        for (int j = 0; j < N; ++j) {
            colSumA[j] = (colSumA[j] + nums[aStart + rowBase + j]) % MODULO;
            rowSumB[i] = (rowSumB[i] + nums[bStart + rowBase + j]) % MODULO;
        }
    }

    int checksum = 0;
    for (int k = 0; k < N; ++k) {
        checksum = (checksum + colSumA[k] * rowSumB[k]) % MODULO;
    }

    return checksum;
}

bool sendAll(int sock, const string& msg) {
    const char* data = msg.data();
    size_t sentTotal = 0;

    while (sentTotal < msg.size()) {
        ssize_t sent = send(sock, data + sentTotal, msg.size() - sentTotal, 0);
        if (sent <= 0) {
            return false;
        }
        sentTotal += (size_t)sent;
    }

    return true;
}

void parseIncomingInts(const char* buffer, int n, vector<int>& nums, int& currentValue, bool& readingNumber) {
    // Streaming parser: the current number can continue across recv calls.
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
    int port = stoi(argv[2]);
    string team = argv[3];

    cout << "HFT Client\n";
    cout << "Solving matrix checksum challenges.\n\n";

    // --- Connect to server ---
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    // Send team name
    string intro = team + "\n";
    send(sock, intro.c_str(), intro.size(), 0);

    cout << "Connected to server at " << host << ":" << port << "\n";
    cout << "Waiting for challenges...\n";

    char buffer[65536];
    vector<int> nums;
    size_t numStart = 0;
    int currentValue = 0;
    bool readingNumber = false;
    long long parseUsSinceLastAnswer = 0;

    while (true) {
        int n = recv(sock, buffer, sizeof(buffer), 0);          // returns the length of the prompt for each question, every time it will overwrite the buffer and returns n as the length.
        if (n <= 0) {
            cout << "Disconnected from server.\n";
            break;
        }

        auto parseStart = chrono::high_resolution_clock::now();
        // parses from the buffer, loading numbers into nums. Automatically skips over anything that's not digits
        parseIncomingInts(buffer, n, nums, currentValue, readingNumber);
        auto parseEnd = chrono::high_resolution_clock::now();
        parseUsSinceLastAnswer += chrono::duration_cast<chrono::microseconds>(parseEnd - parseStart).count();

        // So there's something to compute
        while (nums.size() - numStart >= 2) {
            // the first number after process is the challenge ID
            int challengeId = nums[numStart];
            // size as the second
            int N = nums[numStart + 1];

            int valuesNeeded = 2 + 2 * N * N;

            if (N <= 0) {
                cerr << "Invalid matrix size: " << N << "\n";
                close(sock);
                return 1;
            }

            // size needed = 1 (ID) + 1 (N) + N * N  + N * N
            if (nums.size() - numStart < (size_t)valuesNeeded) {
                break;
            }

            auto computeStart = chrono::high_resolution_clock::now();
            int answer = computeChecksumFast(nums, (int)numStart, N);
            auto computeEnd = chrono::high_resolution_clock::now();
            auto computeUs = chrono::duration_cast<chrono::microseconds>(computeEnd - computeStart).count();

            string reply = to_string(challengeId) + " " + to_string(answer) + "\n";

            auto sendStart = chrono::high_resolution_clock::now();
            if (!sendAll(sock, reply)) {
                cerr << "Failed to send answer.\n";
                close(sock);
                return 1;
            }
            auto sendEnd = chrono::high_resolution_clock::now();
            auto sendUs = chrono::duration_cast<chrono::microseconds>(sendEnd - sendStart).count();

            cout << "Answered challenge " << challengeId << " with " << answer
                 << " (parse " << parseUsSinceLastAnswer
                 << " us, compute " << computeUs
                 << " us, send " << sendUs << " us)\n";
            parseUsSinceLastAnswer = 0;
            numStart += valuesNeeded;

            // Compact occasionally instead of moving memory after every challenge.
            if (numStart > 100000) {
                nums.erase(nums.begin(), nums.begin() + numStart);
                numStart = 0;
            }
        }
    }

    close(sock);
    return 0;
}
