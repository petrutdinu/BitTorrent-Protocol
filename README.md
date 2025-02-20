# Assignment 2 - The BitTorrent Protocol

## Overview
This project simulates a **BitTorrent-like protocol** using **MPI (Message Passing Interface)** for parallel processing. The implementation consists of multiple processes acting as **peers** and a **tracker**, facilitating file sharing between clients.

## Main Function
The `main` function:
- Initializes the **MPI environment**
- Determines the total number of processes (`numtasks`)
- Identifies each process by **rank**
- The process with `TRACKER_RANK` executes the **tracker** function, while others execute the **peer** function

---
## Workflow

### 1. Initialization of Client and Tracker

#### **Client**
- Reads the input file and extracts details about:
  - Owned files
  - Desired files
- Sends file ownership details to the tracker:
  - Number of files
  - Number of hashes per file
  - Hash values of owned files
- Waits for an acknowledgment (**ACK**) from the tracker before starting downloads

#### **Tracker**
- Receives messages from clients containing file details
- Processes the received files, storing them in a **trackerData** map with a `FileInfo` structure for each file:
  - **If the file already exists** → Updates hashes and segment count
  - **If the file does not exist** → Adds new file details
- Sends **ACK** messages to clients after processing

---
### 2. File Downloading by Clients

#### **Client**
- Creates a local **FileInfo** structure with data received from the tracker
- Stores this in a **DownloadData** structure containing:
  - Client rank
  - Owned files
  - Desired files
  - Local trackerData
- Passes `DownloadData` to a **download thread** (`pthread_t download_thread`)
- The **download thread** follows these steps for each desired file:
  1. Extracts specific file data
  2. Stores existing hashes (if available)
  3. Starts downloading segments:
     - Checks total segments for the file
     - If segment is missing:
       - Every **10 downloaded segments**, requests an updated **swarm** from the tracker (`SWARM` request, tag `6`)
       - Finds a peer in the **swarm** (excluding itself) and sends a **DOWNLOAD** request (tag `5`)
       - Waits for a response:
         - **ACK** → Marks segment as received, updates hash list, and increments the counter
         - **NACK** → Moves to the next peer
  4. Once a file is fully downloaded:
     - Sends **ONE_FILE** message (tag `6`) to the tracker
     - Saves the file with its hashes
  5. After downloading all desired files:
     - Sends **ALL_FILES** message (tag `6`) to the tracker

#### **Tracker**
- Sends **trackerData** details to all clients
- Listens for client messages:
  - **ONE_FILE** → Increments completed file count
  - **ALL_FILES** → Increments fully completed clients count
  - If all clients finish, sends **SHUTDOWN** message (tag `9`) and terminates
  - **SWARM requests** → Sends updated swarm data

---
### 3. File Uploading by Clients

#### **Client**
- Creates a **FileInfo** structure with tracker data
- Stores this in a **DownloadData** structure (similar to the download process)
- Passes `DownloadData` to an **upload thread** (`pthread_t upload_thread`)
- The **upload thread** continuously:
  - Waits for messages from other clients or the tracker
  - If it receives a **DOWNLOAD** request:
    - If it owns the segment → Sends **ACK**
    - If it does not own the segment → Sends **NACK**
  - If it receives a **SHUTDOWN** message, it stops

#### **Tracker**
- If all clients have finished downloading, it sends a **SHUTDOWN** message (tag `9`) to all clients and terminates

---
## Communication Tags
| **Tag**   | **Description** |
|-----------|----------------|
| `5`       | DOWNLOAD request |
| `6`       | SWARM update / ONE_FILE / ALL_FILES |
| `9`       | SHUTDOWN signal |

---
## Summary
This project implements a distributed **BitTorrent-like file-sharing protocol** using **MPI and multithreading**. The tracker coordinates clients, while peers manage file exchanges dynamically through **download** and **upload threads**.

### Key Features:
✅ **Peer-to-peer file sharing**  
✅ **Swarm-based segment downloading**  
✅ **Efficient peer selection**  
✅ **Tracker-coordinated file distribution**  
✅ **Multi-threaded download/upload system**  

---
## How to Run
1. **Compile the program** using MPI compiler:
   ```sh
   mpic++ -o tema2 tema2.cpp -pthread -Wall -Werror
   ```
2. **Run the program** with multiple processes:
   ```sh
   mpirun -np <num_processes> ./tema2
   ```

Replace `<num_processes>` with the number of peers + tracker.