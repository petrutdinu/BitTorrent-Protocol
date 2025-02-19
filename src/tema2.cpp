#include <mpi.h>
#include <pthread.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

using namespace std;

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MESSAGE_SIZE 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

// Structură pentru a stoca informațiile unui fișier
struct FileInfo {
    int segmentCount;       // Numărul total de segmente ale fișierului
    vector<string> hashes;  // Hash urile segmentelor fișierului
    vector<int> swarm;      // Lista de ID uri ale clienților care dețin segmentele fișierului
};

// Structură pentru a stoca datele necesare descărcării/ încărcării unui fișier
struct DownloadData {
    int rank;                                // Rank ul clientului
    map<string, vector<string>> filesOwned;  // Fișierele deținute de client
    vector<string> filesWanted;              // Fișierele dorite de client
    map<string, FileInfo> trackerData;       // Informațiile despre fișierele dorite din tracker
};

void* download_thread_func(void* arg) {
    DownloadData* downloadData = (DownloadData*)arg;

    for (const string& fileName : downloadData->filesWanted) {
        FileInfo& fileInfo = downloadData->trackerData[fileName];
        int segmentCount = fileInfo.segmentCount;
        vector<bool> segmentsDownloaded(segmentCount, false);
        vector<string> hashes;
        int downloadedSegmentsCount = 0;

        // Adaugă hash urile deja deținute în lista de hash uri
        if (downloadData->filesOwned.find(fileName) != downloadData->filesOwned.end()) {
            const vector<string>& existingHashes = downloadData->filesOwned[fileName];
            hashes.insert(hashes.end(), existingHashes.begin(), existingHashes.end());
        }

        map<int, int> peerUsageCount;
        // Începe descărcarea segmentelor
        for (int i = 0; i < segmentCount; ++i) {
            if (!segmentsDownloaded[i]) {
                // Cere swarm ul actualizat al fișierului
                if (downloadedSegmentsCount % 10 == 0) {
                    // Trimite mesajul "SWARM" către tracker cu tag ul 6
                    MPI_Send("SWARM", MESSAGE_SIZE, MPI_CHAR, TRACKER_RANK, 6, MPI_COMM_WORLD);

                    // Trimite numele fișierului
                    MPI_Send(fileName.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 7, MPI_COMM_WORLD);

                    // Primește dimensiunea swarm ului
                    int swarmSize;
                    MPI_Recv(&swarmSize, 1, MPI_INT, TRACKER_RANK, 7, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    // Primește lista de clienți din swarm
                    for (int i = 0; i < swarmSize; ++i) {
                        int peerID;
                        MPI_Recv(&peerID, 1, MPI_INT, TRACKER_RANK, 7, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                        // Adaugă clientul în lista locală de swarm, dacă nu există deja
                        if (find(fileInfo.swarm.begin(), fileInfo.swarm.end(), peerID) == fileInfo.swarm.end()) {
                            fileInfo.swarm.push_back(peerID);
                        }
                    }
                }

                vector<int> availablePeers = fileInfo.swarm;
                sort(availablePeers.begin(), availablePeers.end(), [&peerUsageCount](int a, int b) {
                    return peerUsageCount[a] < peerUsageCount[b];  // Sortează în funcție de utilizare
                });

                // Caută segmentul la un peer
                for (int peerID : availablePeers) {
                    if (peerID != downloadData->rank) {
                        // Trimite mesajul "DOWNLOAD" către peer ul ales cu tag ul 5
                        char msg[MESSAGE_SIZE] = "DOWNLOAD";
                        MPI_Send(msg, MESSAGE_SIZE, MPI_CHAR, peerID, 5, MPI_COMM_WORLD);

                        // Trimite hash ul segmentului
                        char segmentHash[HASH_SIZE];
                        strncpy(segmentHash, fileInfo.hashes[i].c_str(), HASH_SIZE);
                        MPI_Send(segmentHash, HASH_SIZE, MPI_CHAR, peerID, 0, MPI_COMM_WORLD);

                        // Primește răspunsul de la peer
                        char recvMsg[MESSAGE_SIZE];
                        MPI_Recv(recvMsg, MESSAGE_SIZE, MPI_CHAR, peerID, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                        // Dacă primește "ACK" de la peer, adaugă segmentul la lista de segmente descărcate
                        if (strcmp(recvMsg, "ACK") == 0) {
                            segmentsDownloaded[i] = true;
                            downloadedSegmentsCount++;
                            hashes.push_back(fileInfo.hashes[i]);
                            peerUsageCount[peerID]++;
                            break;
                        } else if (strcmp(recvMsg, "NACK") == 0) {
                            // Dacă primește "NACK" de la peer, încearcă la următorul peer
                            continue;
                        }
                    }
                }
            }
        }

        // Informează trackerul că a terminat descărcarea unui fișier
        // Trimite mesajul "ONE_FILE" către tracker cu tag ul 6
        MPI_Send("ONE_FILE", MESSAGE_SIZE, MPI_CHAR, TRACKER_RANK, 6, MPI_COMM_WORLD);

        // Salvează fișierul descărcat
        string clientFileName = "client" + to_string(downloadData->rank) + "_" + fileName;
        ofstream outFile(clientFileName);

        if (outFile.is_open()) {
            for (const string& hash : hashes) {
                outFile << hash << "\n";
            }
            outFile.close();
        } else {
            cerr << "Eroare la salvarea fisierului " << fileName << endl;
        }
    }

    // Informează trackerul că a terminat descărcarea tuturor fișierelor
    // Trimite mesajul "ALL_FILES" către tracker cu tag ul 6
    MPI_Send("ALL_FILES", MESSAGE_SIZE, MPI_CHAR, TRACKER_RANK, 6, MPI_COMM_WORLD);

    return nullptr;
}

void* upload_thread_func(void* arg) {
    DownloadData* downloadData = (DownloadData*)arg;

    while (true) {
        MPI_Status status;

        // Poate primi fie mesajul "DOWNLOAD" de la un client, fie mesajul "SHUTDOWN" de la tracker
        char recvMsg[MESSAGE_SIZE];
        MPI_Recv(recvMsg, MESSAGE_SIZE, MPI_CHAR, MPI_ANY_SOURCE, 5, MPI_COMM_WORLD, &status);

        if (strcmp(recvMsg, "DOWNLOAD") == 0) {
            char segmentHash[HASH_SIZE];
            MPI_Recv(segmentHash, HASH_SIZE, MPI_CHAR, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

            // Verifică dacă deține segmentul solicitat
            bool hasSegment = false;
            for (const auto& file : downloadData->filesOwned) {
                for (const auto& hash : file.second) {
                    if (hash == string(segmentHash, HASH_SIZE)) {
                        hasSegment = true;
                        break;
                    }
                }
                if (hasSegment) break;
            }

            // Dacă îl deține, îl trimite la clientul care a făcut cererea
            if (hasSegment) {
                MPI_Send("ACK", MESSAGE_SIZE, MPI_CHAR, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
            } else {
                // Dacă nu îl deține, îi trimite clientului un mesaj negativ
                MPI_Send("NACK", MESSAGE_SIZE, MPI_CHAR, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
            }
        } else if (strcmp(recvMsg, "SHUTDOWN") == 0) {
            // Dacă primește mesajul "SHUTDOWN" de la tracker, se oprește
            break;
        }
    }

    return nullptr;
}

void tracker(int numtasks, int rank) {
    map<string, FileInfo> trackerData;

    // Așteaptă mesajele de la fiecare client
    for (int i = 1; i < numtasks; ++i) {
        int filesOwnedCount;
        MPI_Recv(&filesOwnedCount, 1, MPI_INT, i, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // Procesează fișierele deținute de client
        for (int j = 0; j < filesOwnedCount; ++j) {
            char fileName[MAX_FILENAME];
            MPI_Recv(fileName, MAX_FILENAME, MPI_CHAR, i, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            int segmentCount;
            MPI_Recv(&segmentCount, 1, MPI_INT, i, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            vector<string> hashes(segmentCount);
            for (int k = 0; k < segmentCount; ++k) {
                char hash[HASH_SIZE];
                MPI_Recv(hash, HASH_SIZE, MPI_CHAR, i, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                hashes[k] = string(hash, HASH_SIZE);
            }

            // Adaugă doar hash urile care nu există deja în lista de hash uri
            if (trackerData.find(fileName) != trackerData.end()) {
                FileInfo& fileInfo = trackerData[fileName];
                for (const string& hash : hashes) {
                    if (find(fileInfo.hashes.begin(), fileInfo.hashes.end(), hash) == fileInfo.hashes.end()) {
                        fileInfo.hashes.push_back(hash);
                        fileInfo.segmentCount++;
                    }
                }
                fileInfo.swarm.push_back(i);
            } else {
                // Actualizează trackerData cu fișierul și segmentul său
                FileInfo fileInfo;
                fileInfo.segmentCount = segmentCount;
                fileInfo.hashes = hashes;
                fileInfo.swarm.push_back(i);
                trackerData[fileName] = fileInfo;
            }
        }
    }

    // Trimite mesajul "ACK" fiecărui client
    for (int i = 1; i < numtasks; ++i) {
        MPI_Send("ACK", MESSAGE_SIZE, MPI_CHAR, i, 1, MPI_COMM_WORLD);
    }

    // Trimite numărul total de fișiere către fiecare client
    int totalFiles = trackerData.size();
    for (int i = 1; i < numtasks; ++i) {
        MPI_Send(&totalFiles, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
    }

    // Trimite datele din trackerData către fiecare client
    for (int i = 1; i < numtasks; ++i) {
        for (const auto& entry : trackerData) {
            const string& fileName = entry.first;
            const FileInfo& fileInfo = entry.second;

            // Trimite numele fișierului
            int fileNameLength = fileName.size() + 1;
            MPI_Send(&fileNameLength, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
            MPI_Send(fileName.c_str(), fileNameLength, MPI_CHAR, i, 0, MPI_COMM_WORLD);

            // Trimite numărul de segmente
            MPI_Send(&fileInfo.segmentCount, 1, MPI_INT, i, 0, MPI_COMM_WORLD);

            // Trimite hash urile segmentelor
            for (const string& hash : fileInfo.hashes) {
                MPI_Send(hash.c_str(), HASH_SIZE, MPI_CHAR, i, 0, MPI_COMM_WORLD);
            }

            // Trimite lista de clienți din swarm
            int swarmSize = fileInfo.swarm.size();
            MPI_Send(&swarmSize, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
            for (int peerID : fileInfo.swarm) {
                MPI_Send(&peerID, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
            }
        }
    }

    int nrReady = 0;
    int nrDone = 0;

    while (true) {
        MPI_Status status;

        // Poate primi fie mesajul "ONE_FILE", fie mesajul "ALL_FILES", fie mesajul "SWARM" de la un client
        char msg[MESSAGE_SIZE];
        MPI_Recv(msg, MESSAGE_SIZE, MPI_CHAR, MPI_ANY_SOURCE, 6, MPI_COMM_WORLD, &status);

        if (strcmp(msg, "ONE_FILE") == 0) {
            // Incremneatză numărul de clienți care au terminat descărcarea unui fișier
            nrReady++;
        } else if (strcmp(msg, "ALL_FILES") == 0) {
            // Incremnează numărul de clienți care au terminat descărcarea tuturor fișierelor
            nrDone++;

            // Dacă toți clienții au terminat descărcarea, trimite mesajul "SHUTDOWN" către fiecare client
            if (nrDone == numtasks - 1) {
                for (int i = 1; i < numtasks; ++i) {
                    MPI_Send("SHUTDOWN", 9, MPI_CHAR, i, 5, MPI_COMM_WORLD);
                }
                break;
            }
        } else if (strcmp(msg, "SWARM") == 0) {
            // Dacă primește mesajul "SWARM" de la un client, trimite informațiile despre swarm ul fișierului
            char fileName[MAX_FILENAME];
            MPI_Recv(fileName, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, 7, MPI_COMM_WORLD, &status);

            FileInfo& fileInfo = trackerData[fileName];

            // Trimite dimensiunea swarm ului
            int swarmSize = fileInfo.swarm.size();
            MPI_Send(&swarmSize, 1, MPI_INT, status.MPI_SOURCE, 7, MPI_COMM_WORLD);

            // Trimite lista de clienți din swarm
            for (int peerID : fileInfo.swarm) {
                MPI_Send(&peerID, 1, MPI_INT, status.MPI_SOURCE, 7, MPI_COMM_WORLD);
            }

            // Adaugă clientul care a trimis mesajul "SWARM" la lista de clienți din swarm ul fișierului
            if (find(fileInfo.swarm.begin(), fileInfo.swarm.end(), status.MPI_SOURCE) == fileInfo.swarm.end()) {
                fileInfo.swarm.push_back(status.MPI_SOURCE);
            }
        }
    }
}

void peer(int numtasks, int rank) {
    // Deschide fișierul de intrare pentru acest client
    ifstream inputFile("in" + to_string(rank) + ".txt");
    if (!inputFile.is_open()) {
        cerr << "Eroare la deschiderea fisierului de input pentru clientul " << rank << endl;
        exit(-1);
    }

    // Citește și stochează fișierele deținute
    int filesOwnedCount;
    inputFile >> filesOwnedCount;

    map<string, vector<string>> filesOwned;
    for (int i = 0; i < filesOwnedCount; ++i) {
        string fileName;
        inputFile >> fileName;

        int segmentCount;
        inputFile >> segmentCount;

        vector<string> segments(segmentCount);
        for (int j = 0; j < segmentCount; ++j) {
            inputFile >> segments[j];
        }

        filesOwned[fileName] = segments;
    }

    // Citește și stochează fișierele dorite
    int filesWantedCount;
    inputFile >> filesWantedCount;

    vector<string> filesWanted(filesWantedCount);
    for (int i = 0; i < filesWantedCount; ++i) {
        inputFile >> filesWanted[i];
    }
    inputFile.close();

    // Trimite numărul de fișiere deținute către tracker
    MPI_Send(&filesOwnedCount, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);

    // Trimite fișierele deținute către tracker
    for (const auto& entry : filesOwned) {
        const string& fileName = entry.first;
        const vector<string>& hashes = entry.second;
        int segmentCount = hashes.size();

        // Trimite informațiile despre fișier către tracker
        MPI_Send(fileName.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
        MPI_Send(&segmentCount, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);

        for (const string& hash : hashes) {
            MPI_Send(hash.c_str(), HASH_SIZE, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
        }
    }

    // Primește mesajul "ACK" de la tracker
    char recvMsg[MESSAGE_SIZE];
    MPI_Recv(recvMsg, MESSAGE_SIZE, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (strcmp(recvMsg, "ACK") != 0) {
        cerr << "Eroare la primirea mesajului ACK de la tracker" << endl;
        exit(-1);
    }

    // Primește numărul de fișiere din tracker
    int totalFiles;
    MPI_Recv(&totalFiles, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // Primește informațiile fiecărui fișier din tracker
    map<string, FileInfo> localTrackerData;
    for (int i = 0; i < totalFiles; ++i) {
        // Primește numele fișierului
        int fileNameLength;
        MPI_Recv(&fileNameLength, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        char fileName[fileNameLength];
        MPI_Recv(fileName, fileNameLength, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        fileName[fileNameLength] = '\0';

        // Primește numărul de segmente
        int segmentCount;
        MPI_Recv(&segmentCount, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // Primește hash urile segmentelor
        vector<string> hashes(segmentCount);
        for (int j = 0; j < segmentCount; ++j) {
            hashes[j].resize(HASH_SIZE);
            MPI_Recv(&hashes[j][0], HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        // Primește lista de clienți din swarm
        int swarmSize;
        MPI_Recv(&swarmSize, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        vector<int> swarm(swarmSize);
        for (int j = 0; j < swarmSize; ++j) {
            MPI_Recv(&swarm[j], 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        // Stochează informațiile primite într o structură locală
        localTrackerData[fileName] = FileInfo{
            segmentCount,  // Numărul total de segmente
            hashes,        // Hash urile segmentelor
            swarm          // Lista de clienți care dețin fișierul
        };
    }

    // Creează structura DownloadData
    DownloadData downloadData;
    downloadData.rank = rank;
    downloadData.filesOwned = filesOwned;
    downloadData.filesWanted = filesWanted;
    downloadData.trackerData = localTrackerData;

    // Creează thread urile de download și upload
    pthread_t download_thread;
    pthread_t upload_thread;
    void* status;
    int r;

    r = pthread_create(&download_thread, nullptr, download_thread_func, (void*)&downloadData);
    if (r) {
        cerr << "Eroare la crearea thread-ului de download" << endl;
        exit(-1);
    }

    r = pthread_create(&upload_thread, nullptr, upload_thread_func, (void*)&downloadData);
    if (r) {
        cerr << "Eroare la crearea thread-ului de upload" << endl;
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        cerr << "Eroare la asteptarea thread-ului de download" << endl;
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        cerr << "Eroare la asteptarea thread-ului de upload" << endl;
        exit(-1);
    }
}

int main(int argc, char* argv[]) {
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        cerr << "MPI nu are suport pentru multi-threading" << endl;
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}