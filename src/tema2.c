#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_NUMTASKS 100 

typedef enum {
    MSG_ACK = 1,         // Confirmare (Acknowledgement)
    MSG_REQUEST = 2,         // Cerere (Request)
    MSG_SEGMENT = 3,         // Segment
    MSG_END_OF_MESSAGE = 4,  // Sfârșit de mesaj (End of Message)
    MSG_UPDATE = 5,          // Actualizare (Update)
    MSG_FINISH = 6,          // Finalizare (Finish)
    MSG_TERMINATE = 7       // Sfârșit (Terminate)
} MessageType;

typedef struct  {
    int file_number;                           // ID-ul fișierului
    int n_segments;                    // Numărul de segmente
    char (*segments)[HASH_SIZE + 1];   // Hash-ul fiecărui segment
    int usage_count;                  // Numărul de utilizări pentru fiecare segment
} file_info;


typedef struct {
    int rank;
    int number_of_files;
    int number_of_tasks;
} Peer_args;

file_info* users_files;
file_info* wish_list;



typedef struct TrackerData {
    file_info** all_files;
    int** swarms;
    int** seeds;
    int number_of_tasks;
    int n_clients;
} TrackerData;

// Inițializare structuri tracker
TrackerData* init_tracker(int number_of_tasks) {
    TrackerData* data = calloc(1, sizeof(TrackerData));
    if (!data) {
        return NULL;
    }
    data->number_of_tasks = number_of_tasks;
    data->n_clients = number_of_tasks - 1;

    // Alocare swarms
    data->swarms = calloc(MAX_FILES + 1, sizeof(int*));
    if (!data->swarms) goto cleanup_data;
    
    for (int i = 0; i < MAX_FILES + 1; i++) {
        data->swarms[i] = calloc(number_of_tasks, sizeof(int));
        if (!data->swarms[i]) goto cleanup_swarms;
    }

    // Alocare seeds
    data->seeds = calloc(MAX_FILES + 1, sizeof(int*));
    if (!data->seeds) goto cleanup_swarms;
    
    for (int i = 0; i < MAX_FILES + 1; i++) {
        data->seeds[i] = calloc(number_of_tasks, sizeof(int));
        if (!data->seeds[i]) goto cleanup_seeds;
    }

    // Alocare all_files
    data->all_files = calloc(number_of_tasks, sizeof(file_info*));
    if (!data->all_files) goto cleanup_seeds;
    
    for (int i = 0; i < number_of_tasks; i++) {
        data->all_files[i] = calloc(MAX_FILES + 1, sizeof(file_info));
        if (!data->all_files[i]) goto cleanup_files;

        for (int j = 0; j < MAX_FILES + 1; j++) {
            data->all_files[i][j].segments = calloc(MAX_CHUNKS, sizeof(char[HASH_SIZE + 1]));
            if (!data->all_files[i][j].segments) goto cleanup_segments;
        }
    }

    return data;

cleanup_segments:
    for (int i = 0; i < number_of_tasks; i++) {
        for (int j = 0; j < MAX_FILES + 1; j++) {
            free(data->all_files[i][j].segments);
        }
    }
cleanup_files:
    for (int i = 0; i < number_of_tasks; i++) {
        free(data->all_files[i]);
    }
    free(data->all_files);
cleanup_seeds:
    for (int i = 0; i < MAX_FILES + 1; i++) {
        free(data->seeds[i]);
    }
    free(data->seeds);
cleanup_swarms:
    for (int i = 0; i < MAX_FILES + 1; i++) {
        free(data->swarms[i]);
    }
    free(data->swarms);
cleanup_data:
    free(data);
    return NULL;
}

// Primire fișiere inițiale de la clienți
#define CHECK_MPI(call) \
    do { \
        int ret = (call); \
        if (ret != MPI_SUCCESS) { \
            fprintf(stderr, "MPI error at %s:%d\n", __FILE__, __LINE__); \
            MPI_Abort(MPI_COMM_WORLD, ret); \
        } \
    } while (0)


// Validează ID-ul fișierului
static inline int validate_file_id(int file_id, int sender) {
    if (file_id < 0 || file_id > MAX_FILES) {
        fprintf(stderr, "Invalid file_id %d from sender %d\n", file_id, sender);
        return 0;
    }
    return 1;
}

// Primește și procesează informațiile despre un segment
static int receive_segment_info(TrackerData* data, int file_id, int sender, int segment_id) {
    if (!data->all_files[sender][file_id].segments) {
        fprintf(stderr, "Null segments array for file %d from sender %d\n", file_id, sender);
        return -1;
    }

    CHECK_MPI(MPI_Recv(data->all_files[sender][file_id].segments[segment_id],
                      HASH_SIZE + 1, MPI_CHAR, sender, 0,
                      MPI_COMM_WORLD, MPI_STATUS_IGNORE));

    // Verifică dacă hash-ul primit este valid
    if (strlen(data->all_files[sender][file_id].segments[segment_id]) != HASH_SIZE) {
        fprintf(stderr, "Invalid hash length for file %d, segment %d from sender %d\n",
                file_id, segment_id, sender);
        return -1;
    }

    return 0;
}

// Primește și procesează informațiile despre un fișier
static int receive_file_info(TrackerData* data, int sender) {
    int file_id;
    CHECK_MPI(MPI_Recv(&file_id, 1, MPI_INT, sender, 0, 
                      MPI_COMM_WORLD, MPI_STATUS_IGNORE));
    
    if (!validate_file_id(file_id, sender)) {
        return -1;
    }

    // Marchează sender-ul în swarm și seeds
    data->swarms[file_id][sender] = 1;
    data->seeds[file_id][sender] = 1;

    // Primește numărul de segmente
    int n_segments;
    CHECK_MPI(MPI_Recv(&n_segments, 1, MPI_INT, sender, 0,
                      MPI_COMM_WORLD, MPI_STATUS_IGNORE));

    if (n_segments <= 0 || n_segments > MAX_CHUNKS) {
        fprintf(stderr, "Invalid number of segments %d for file %d from sender %d\n",
                n_segments, file_id, sender);
        return -1;
    }

    // Actualizează informațiile despre fișier
    data->all_files[sender][file_id].file_number = file_id;
    data->all_files[sender][file_id].n_segments = n_segments;

    // Actualizează numărul de segmente pentru toți clienții
    for (int k = 1; k < data->number_of_tasks; k++) {
        data->all_files[k][file_id].n_segments = n_segments;
    }

    // Primește hash-urile pentru toate segmentele
    for (int k = 0; k < n_segments; k++) {
        if (receive_segment_info(data, file_id, sender, k) < 0) {
            return -1;
        }
    }

    return 0;
}

void receive_initial_files(TrackerData* data) {
    if (!data) {
        fprintf(stderr, "Invalid tracker data pointer\n");
        return;
    }

    int successful_receptions = 0;
    int expected_receptions = data->number_of_tasks - 1;

    while (successful_receptions < expected_receptions) {
        MPI_Status status;
        int number_of_files = 0;

        // Primește numărul de fișiere de la un client
        CHECK_MPI(MPI_Recv(&number_of_files, 1, MPI_INT, MPI_ANY_SOURCE, 0, 
                          MPI_COMM_WORLD, &status));
        int sender = status.MPI_SOURCE;

        if (number_of_files < 0 || number_of_files > MAX_FILES) {
            fprintf(stderr, "Invalid number of files %d from sender %d\n", number_of_files, sender);
            continue;
        }

        // Procesează fiecare fișier
        int files_processed = 0;
        for (int j = 0; j < number_of_files; j++) {
            if (receive_file_info(data, sender) == 0) {
                files_processed++;
            }
        }

        if (files_processed == number_of_files) {
            successful_receptions++;
        } else {
            fprintf(stderr, "Only %d/%d files successfully processed from sender %d\n",
                    files_processed, number_of_files, sender);
        }
    }

    fprintf(stderr, "Successfully received files from all %d clients\n",
            successful_receptions);
}

// Funcție auxiliară pentru a trimite detaliile unui segment către un client
void send_segment_request(int sender, int segment_id, int peer_id, const char* segment_hash) {
    int signal = MSG_SEGMENT;
    CHECK_MPI(MPI_Send(&signal, 1, MPI_INT, sender, 0, MPI_COMM_WORLD));
    CHECK_MPI(MPI_Send(&segment_id, 1, MPI_INT, sender, 0, MPI_COMM_WORLD));
    CHECK_MPI(MPI_Send(&peer_id, 1, MPI_INT, sender, 0, MPI_COMM_WORLD));
    CHECK_MPI(MPI_Send(segment_hash, HASH_SIZE + 1, MPI_CHAR, sender, 0, MPI_COMM_WORLD));
}

// Procesare cerere segment
void handle_segment_request1(TrackerData* data, int sender) {
    int file_id;
    CHECK_MPI(MPI_Recv(&file_id, 1, MPI_INT, sender, 0, 
                      MPI_COMM_WORLD, MPI_STATUS_IGNORE));

    if (file_id < 0 || file_id > MAX_FILES) {
        fprintf(stderr, "Invalid file_id %d in request\n", file_id);
        return;
    }

    // Trimite numărul de segmente
    for (int i = 1; i < data->number_of_tasks; i++) {
        if (data->swarms[file_id][i]) {
            CHECK_MPI(MPI_Send(&data->all_files[i][file_id].n_segments, 1, MPI_INT,
                              sender, 0, MPI_COMM_WORLD));
            break;
        }
    }

    // Trimite lista de peers și segmentele lor
    for (int i = 1; i < data->number_of_tasks; i++) {
        if ((data->swarms[file_id][i] || data->seeds[file_id][i]) && i != sender) {
            for (int j = 0; j < data->all_files[i][file_id].n_segments; j++) {
                if (strlen(data->all_files[i][file_id].segments[j]) > 0) {
                    send_segment_request(sender, j, i, data->all_files[i][file_id].segments[j]);
                }
            }
        }
    }

    int signal = MSG_END_OF_MESSAGE;
    CHECK_MPI(MPI_Send(&signal, 1, MPI_INT, sender, 0, MPI_COMM_WORLD));
}


// Procesare actualizare de la client
void handle_update(TrackerData* data, int sender) {
    while (1) {
        int signal;
        CHECK_MPI(MPI_Recv(&signal, 1, MPI_INT, sender, 1, 
                          MPI_COMM_WORLD, MPI_STATUS_IGNORE));

        if (signal == MSG_END_OF_MESSAGE) break;

        if (signal == MSG_SEGMENT) {
            int segment_id, file_id;
            CHECK_MPI(MPI_Recv(&segment_id, 1, MPI_INT, sender, 0,
                              MPI_COMM_WORLD, MPI_STATUS_IGNORE));
            CHECK_MPI(MPI_Recv(&file_id, 1, MPI_INT, sender, 0,
                              MPI_COMM_WORLD, MPI_STATUS_IGNORE));

            if (file_id < 0 || file_id > MAX_FILES ||
                segment_id < 0 || segment_id >= MAX_CHUNKS) {
                fprintf(stderr, "Invalid update: file_id=%d, segment_id=%d\n",
                        file_id, segment_id);
                continue;
            }

            CHECK_MPI(MPI_Recv(data->all_files[sender][file_id].segments[segment_id],
                              HASH_SIZE + 1, MPI_CHAR, sender, 0,
                              MPI_COMM_WORLD, MPI_STATUS_IGNORE));
            data->swarms[file_id][sender] = 1;
        }
    }
}

// Eliberare memorie
void cleanup_tracker(TrackerData* data) {
    if (!data) return;

    if (data->all_files) {
        for (int i = 0; i < data->number_of_tasks; i++) {
            if (data->all_files[i]) {
                for (int j = 0; j < MAX_FILES + 1; j++) {
                    free(data->all_files[i][j].segments);
                }
                free(data->all_files[i]);
            }
        }
        free(data->all_files);
    }

    if (data->swarms) {
        for (int i = 0; i < MAX_FILES + 1; i++) {
            free(data->swarms[i]);
        }
        free(data->swarms);
    }

    if (data->seeds) {
        for (int i = 0; i < MAX_FILES + 1; i++) {
            free(data->seeds[i]);
        }
        free(data->seeds);
    }

    free(data);
}

void tracker(int number_of_tasks, int rank) {
    // Inițializare tracker
    TrackerData* data = init_tracker(number_of_tasks);
    if (!data) {
        fprintf(stderr, "Failed to initialize tracker\n");
        return;
    }

    // Primire fișiere inițiale
    receive_initial_files(data);

    // Trimite semnal de start către toți clienții
    int signal = MSG_ACK;
    for (int i = 1; i < number_of_tasks; i++) {
        CHECK_MPI(MPI_Send(&signal, 1, MPI_INT, i, 0, MPI_COMM_WORLD));
    }

    // Loop principal
    while (data->n_clients > 0) {
        MPI_Status status;
        CHECK_MPI(MPI_Recv(&signal, 1, MPI_INT, MPI_ANY_SOURCE, 1,
                          MPI_COMM_WORLD, &status));
        int sender = status.MPI_SOURCE;

        switch (signal) {
            case MSG_REQUEST:
                handle_segment_request1(data, sender);
                break;

            case MSG_UPDATE:
                handle_update(data, sender);
                break;

            case MSG_FINISH: {
                int file_id;
                CHECK_MPI(MPI_Recv(&file_id, 1, MPI_INT, sender, 0,
                                  MPI_COMM_WORLD, MPI_STATUS_IGNORE));

                if (file_id >= 0 && file_id <= MAX_FILES) {
                    for (int i = 1; i < number_of_tasks; i++) {
                        if (data->seeds[file_id][i]) {
                            for (int j = 0; j < data->all_files[i][file_id].n_segments; j++) {
                                strncpy(data->all_files[sender][file_id].segments[j],
                                      data->all_files[i][file_id].segments[j],
                                      HASH_SIZE);
                                data->all_files[sender][file_id].segments[j][HASH_SIZE] = '\0';
                            }
                        }
                    }
                    data->seeds[file_id][sender] = 1;
                }
                break;
            }

            case MSG_TERMINATE:
                data->n_clients--;
                break;
        }
    }

    // Trimite semnal de terminare către toți clienții
    signal = MSG_TERMINATE;
    for (int i = 1; i < number_of_tasks; i++) {
        CHECK_MPI(MPI_Send(&signal, 1, MPI_INT, i, 1, MPI_COMM_WORLD));
    }

    cleanup_tracker(data);
}

// receives list from the tracker with all peers/seeds from which
// the client can request a segment; peer_list contains the hashes
// of that every client has from the requested file


typedef struct PeerListConfig {
    int number_of_tasks;
    int file_id;
    int n_segments;
} PeerListConfig;

// Funcție pentru curățarea memoriei alocate
static void cleanup_peer_list(file_info* peer_list, int count) {
    if (!peer_list) return;
    
    for (int i = 0; i < count; i++) {
        free(peer_list[i].segments);
    }
    free(peer_list);
}

// Funcție pentru inițializarea listei de peer-uri
static file_info* init_peer_list(const PeerListConfig* config) {
    file_info* peer_list = calloc(config->number_of_tasks, sizeof(file_info));
    if (!peer_list) {
        fprintf(stderr, "Failed to allocate peer list\n");
        return NULL;
    }

    for (int i = 0; i < config->number_of_tasks; i++) {
        peer_list[i].file_number = config->file_id;
        peer_list[i].n_segments = config->n_segments;
        peer_list[i].segments = calloc(config->n_segments, sizeof(char[HASH_SIZE + 1]));
        
        if (!peer_list[i].segments) {
            fprintf(stderr, "Failed to allocate segments for peer %d\n", i);
            cleanup_peer_list(peer_list, i);
            return NULL;
        }
    }
    
    return peer_list;
}

// Funcție pentru procesarea unui segment primit
static int process_segment(file_info* peer_list, const PeerListConfig* config) {
    int segment_id, peer_id;
    
    // Primirea detaliilor segmentului
    CHECK_MPI(MPI_Recv(&segment_id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE));
    CHECK_MPI(MPI_Recv(&peer_id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE));
    
    // Validarea indicilor primiți
    if (segment_id < 0 || segment_id >= config->n_segments ||
        peer_id < 0 || peer_id >= config->number_of_tasks) {
        fprintf(stderr, "Invalid indices: segment=%d, peer=%d\n", segment_id, peer_id);
        return 0;
    }
    
    // Primirea hash-ului segmentului
    CHECK_MPI(MPI_Recv(peer_list[peer_id].segments[segment_id], 
                      HASH_SIZE + 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD, 
                      MPI_STATUS_IGNORE));
    
    return 1;
}

// Funcție pentru verificarea datelor primite
static int validate_received_data(const file_info* peer_list, const PeerListConfig* config) {
    for (int i = 0; i < config->number_of_tasks; i++) {
        for (int j = 0; j < config->n_segments; j++) {
            if (strlen(peer_list[i].segments[j]) > 0) {
                return 1;
            }
        }
    }
    return 0;
}

// Funcția principală pentru obținerea listei de peer-uri
file_info* getPeerList(int number_of_tasks, file_info current_file) {
    // Validare parametri de intrare
    if (number_of_tasks <= 0 || current_file.n_segments <= 0) {
        fprintf(stderr, "Invalid parameters: number_of_tasks=%d, segments=%d\n",
                number_of_tasks, current_file.n_segments);
        return NULL;
    }

    // Inițializare configurație
    PeerListConfig config = {
        .number_of_tasks = number_of_tasks,
        .file_id = current_file.file_number,
        .n_segments = current_file.n_segments
    };

    // Inițializare listă peer-uri
    file_info* peer_list = init_peer_list(&config);
    if (!peer_list) return NULL;

    // Procesare mesaje de la tracker
    int continue_receiving = 1;
    while (continue_receiving) {
        int signal;
        MPI_Status status;
        
        CHECK_MPI(MPI_Recv(&signal, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, &status));

        switch (signal) {
            case MSG_SEGMENT:
                if (!process_segment(peer_list, &config)) {
                    cleanup_peer_list(peer_list, config.number_of_tasks);
                    return NULL;
                }
                break;

            case MSG_END_OF_MESSAGE:
                continue_receiving = 0;
                break;

            default:
                fprintf(stderr, "Unknown signal received: %d\n", signal);
                cleanup_peer_list(peer_list, config.number_of_tasks);
                return NULL;
        }
    }

    // Verificare date valide
    if (!validate_received_data(peer_list, &config)) {
        fprintf(stderr, "Warning: No valid segments received\n");
    }

    return peer_list;
}



// Helper function to update tracker with current segments
void send_segment_update(int file_id, const file_info *owned_file) {
    int signal = MSG_UPDATE;
    MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
    
    for (int j = 0; j < owned_file->n_segments; j++) {
        if (strlen(owned_file->segments[j]) > 0) {
            signal = MSG_SEGMENT;
            MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
            MPI_Send(&j, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
            MPI_Send(&file_id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
            MPI_Send(owned_file->segments[j], HASH_SIZE + 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
        }
    }
    
    signal = MSG_END_OF_MESSAGE;
    MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
}

// Helper function to download a segment from a peer
int download_segment_from_peer(int peer_rank, const char* segment_hash) {
    int signal = MSG_REQUEST;
    int max_retries = 3;
    int retry_count = 0;
    
    while (retry_count < max_retries) {
        MPI_Send(&signal, 1, MPI_INT, peer_rank, 1, MPI_COMM_WORLD);
        MPI_Send(segment_hash, HASH_SIZE + 1, MPI_CHAR, peer_rank, 1, MPI_COMM_WORLD);
        
        MPI_Recv(&signal, 1, MPI_INT, peer_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        if (signal == MSG_ACK) {
            return 1; // Success
        }
        retry_count++;
    }
    return 0; // Failed after retries
}

// Helper function to save downloaded file
void save_downloaded_file(int rank, int file_id, const file_info *owned_file) {
    char output_file[MAX_FILENAME];
    sprintf(output_file, "client%d_file%d", rank, file_id);
    
    FILE* new_file = fopen(output_file, "w");
    if (new_file == NULL) {
        fprintf(stderr, "Error opening file %s for writing\n", output_file);
        return;
    }
    
    for (int k = 0; k < owned_file->n_segments; k++) {
        fprintf(new_file, "%s\n", owned_file->segments[k]);
    }
    fclose(new_file);
}

// Main download thread function
void *download_thread_func(void *arg) {
    Peer_args args = *(Peer_args *)arg;
    int rank = args.rank;
    int number_of_files = args.number_of_files;
    int number_of_tasks = args.number_of_tasks;

    // Procesare pentru fiecare fișier dorit
    for (int file_idx = 0; file_idx < number_of_files; file_idx++) {
        int signal = MSG_REQUEST;
        int current_file_id = wish_list[file_idx].file_number;

        file_info current_file = {.file_number = current_file_id};

        // Trimitere cerere către tracker
        MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
        MPI_Send(&current_file_id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);

        // Obținere informații fișier
        MPI_Recv(&current_file.n_segments, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        users_files[current_file_id].file_number = current_file_id;
        users_files[current_file_id].n_segments = current_file.n_segments;

        // Obținere lista de peers
        file_info *peer_list = getPeerList(number_of_tasks, current_file);
        if (!peer_list) {
            fprintf(stderr, "Failed to get peer list for file %d\n", current_file_id);
            continue;
        }

        int segments_processed = 0;

        // Descărcare segmente
        for (int seg = 0; seg < current_file.n_segments; seg++) {
            if (segments_processed == MAX_FILES) {
                send_segment_update(current_file_id, &users_files[current_file_id]);

                // Obținere listă de peers actualizată
                for (int i = 0; i < number_of_tasks; i++) {
                    free(peer_list[i].segments);
                }
                free(peer_list);

                signal = MSG_REQUEST;
                MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
                MPI_Send(&current_file_id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
                MPI_Recv(&current_file.n_segments, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                peer_list = getPeerList(number_of_tasks, current_file);
                segments_processed = 0;
            }

            if (strlen(users_files[current_file_id].segments[seg]) > 0) {
                continue; // Segment deja descărcat
            }

            // Găsire peer optim pentru descărcare
            int chosen_peer = -1;

            for (int p = 1; p < number_of_tasks; p++) {
                if (p != rank && strlen(peer_list[p].segments[seg]) > 0) {
                    chosen_peer = p;
                    break; // Selectăm primul peer disponibil
                }
            }

            if (chosen_peer == -1) {
                fprintf(stderr, "No available peers for segment %d of file %d\n", seg, current_file_id);
                continue;
            }

            // Descărcare segment
            if (download_segment_from_peer(chosen_peer, peer_list[chosen_peer].segments[seg])) {
                strcpy(users_files[current_file_id].segments[seg], peer_list[chosen_peer].segments[seg]);
                segments_processed++;
            }
        }

        // Notificare tracker despre completare
        signal = MSG_FINISH;
        MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
        MPI_Send(&current_file_id, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);

        // Salvare fișier și curățare
        save_downloaded_file(rank, current_file_id, &users_files[current_file_id]);

        for (int i = 0; i < number_of_tasks; i++) {
            free(peer_list[i].segments);
        }
        free(peer_list);
    }

    // Semnalizare finalizare
    int signal = MSG_TERMINATE;
    MPI_Send(&signal, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);

    return NULL;
}




void handle_segment_request(int sender_rank, char *requested_hash) {
    int signal = -1;
    int found = 0;

    // caută segmentul în lista de segmente a fiecărui fișier
    for (int file_id = 1; file_id <= MAX_FILES && !found; file_id++) {
        if (users_files[file_id].n_segments > 0) {
            for (int seg = 0; seg < users_files[file_id].n_segments; seg++) {
                // Check if the requested segment exists
                if (strcmp(users_files[file_id].segments[seg], requested_hash) == 0) {
                    signal = MSG_ACK;
                    found = 1;
                    break;
                }
            }
        }
    }

    // daca nu a fost gasit segmentul
    if (!found) {
        signal = -1;
    }

    // trimite semnalul înapoi la client
    MPI_Send(&signal, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD);
}

void *upload_thread_func(void *arg) {
    int rank = *(int *)arg; // Identificarea rank-ului clientului
    int is_running = 1;

    while (is_running) {
        int signal = -1; // Inițializarea semnalului primit
        MPI_Status status;

        // Așteptare pentru orice cerere de la alte clienți
        int mpi_ret = MPI_Recv(&signal, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);
        if (mpi_ret != MPI_SUCCESS) {
            fprintf(stderr, "Rank %d: Error receiving signal from MPI. Terminating thread.\n", rank);
            break;
        }

        int sender_rank = status.MPI_SOURCE;

        switch (signal) {
           case MSG_REQUEST: {
                char requested_hash[HASH_SIZE + 1] = {0}; // Inițializare buffer pentru hash-ul cerut

                // Primirea hash-ului segmentului solicitat
                mpi_ret = MPI_Recv(requested_hash, HASH_SIZE + 1, MPI_CHAR, sender_rank, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                if (mpi_ret != MPI_SUCCESS) {
                    fprintf(stderr, "Rank %d: Error receiving hash from rank %d\n", rank, sender_rank);
                    continue;
                }

                // Procesarea cererii pentru segment
                handle_segment_request(sender_rank, requested_hash); 
                break;
            }


            case MSG_TERMINATE:
                // Toți clienții au terminat descărcarea, terminăm thread-ul
                is_running = 0;
                printf("Rank %d: Termination signal received. Shutting down upload thread.\n", rank);
                break;

            default:
                fprintf(stderr, "Rank %d: Unknown signal (%d) received from rank %d. Ignoring.\n", rank, signal, sender_rank);
                break;
        }
    }

    printf("Rank %d: Upload thread terminated.\n", rank);
    return NULL;
}


 

void initialize_users_files(int n_users_files, file_info *fp) {
    // Alocare memorie pentru users_files
    users_files = malloc((MAX_FILES + 1) * sizeof(file_info));
    if (!users_files) {
        fprintf(stderr, "Failed to allocate memory for users_files\n");
        exit(EXIT_FAILURE);
    }

    // Inițializare structură pentru fișierele deținute
    for (int i = 1; i <= MAX_FILES; i++) {
        users_files[i].file_number = 0;
        users_files[i].n_segments = 0;
        users_files[i].segments = malloc(MAX_CHUNKS * sizeof(char[HASH_SIZE + 1]));
        if (!users_files[i].segments) {
            fprintf(stderr, "Failed to allocate memory for segments of file %d\n", i);
            // Curățare și ieșire în caz de eroare
            for (int k = 1; k < i; k++) {
                free(users_files[k].segments);
            }
            free(users_files);
            exit(EXIT_FAILURE);
        }

        // Inițializare segmente cu șiruri goale
        for (int j = 0; j < MAX_CHUNKS; j++) {
            strcpy(users_files[i].segments[j], "");
        }
    }

    // Citirea fișierelor deținute din fișierul de intrare
    for (int i = 0; i < n_users_files; i++) {
        char filename[MAX_FILENAME];
        int n_segments;

        // Citire nume fișier și număr de segmente
        if (fscanf(fp, "%s %d", filename, &n_segments) != 2) {
            fprintf(stderr, "Failed to read owned file information from input file\n");
            continue;
        }

        // Determinare ID fișier din ultimul caracter al numelui
        int file_id = filename[strlen(filename) - 1] - '0';
        if (file_id < 1 || file_id > MAX_FILES) {
            fprintf(stderr, "Invalid file ID %d derived from filename %s\n", file_id, filename);
            continue;
        }

        users_files[file_id].file_number = file_id;
        users_files[file_id].n_segments = n_segments;

        // Citire hash-uri pentru fiecare segment
        for (int j = 0; j < n_segments; j++) {
            if (fscanf(fp, "%s", users_files[file_id].segments[j]) != 1) {
                fprintf(stderr, "Failed to read segment %d of file %d\n", j, file_id);
                break;
            }
        }
    }
}


// Funcție auxiliară pentru a trimite informațiile despre un fișier
void send_file_to_tracker(const file_info* file) {
    MPI_Send(&file->file_number, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
    MPI_Send(&file->n_segments, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
    for (int j = 0; j < file->n_segments; j++) {
        MPI_Send(file->segments[j], HASH_SIZE + 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
    }
}

// Funcție principală pentru trimiterea fișierelor deținute către tracker
void send_users_files_to_tracker(int n_users_files) {
    MPI_Send(&n_users_files, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);

    for (int i = 1; i <= MAX_FILES; i++) {
        if (users_files[i].n_segments > 0) {
            send_file_to_tracker(&users_files[i]);
        }
    }
}


void initialize_wish_list(int n_wish_list, FILE *fp) {
    wish_list = malloc(sizeof(file_info) * n_wish_list);

    for (int i = 0; i < n_wish_list; i++) {
        char filename[MAX_FILENAME];
        fscanf(fp, "%s", filename);

        int file_id = filename[strlen(filename) - 1] - '0';
        wish_list[i].file_number = file_id;
        wish_list[i].n_segments = 0; // Initialize, segments will be fetched during download
        wish_list[i].segments = NULL; // To be assigned during download
    }
}

void wait_for_tracker_confirmation() {
    int signal = -1;
    do {
        MPI_Recv(&signal, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } while (signal != MSG_ACK);
}

void start_threads(int rank, int n_wish_list, int number_of_tasks) {
    pthread_t download_thread, upload_thread;
    Peer_args args = {.rank = rank, .number_of_files = n_wish_list, .number_of_tasks = number_of_tasks};

    if (pthread_create(&download_thread, NULL, download_thread_func, &args) != 0) {
        fprintf(stderr, "Eroare la crearea thread-ului de download\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_create(&upload_thread, NULL, upload_thread_func, &rank) != 0) {
        fprintf(stderr, "Eroare la crearea thread-ului de upload\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_join(download_thread, NULL) != 0) {
        fprintf(stderr, "Eroare la așteptarea thread-ului de download\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_join(upload_thread, NULL) != 0) {
        fprintf(stderr, "Eroare la așteptarea thread-ului de upload\n");
        exit(EXIT_FAILURE);
    }
}

void free_allocated_memory() {
    for (int i = 1; i <= MAX_FILES; i++) {
        free(users_files[i].segments);
    }
    free(users_files);
    free(wish_list);
}

void gestionate_files(int number_of_tasks, int rank) {
    char input_file[MAX_FILENAME];
    sprintf(input_file, "in%d.txt", rank);

    FILE *fp = fopen(input_file, "r");
    if (!fp) {
        fprintf(stderr, "Eroare la deschiderea fișierului de intrare: %s\n", input_file);
        exit(EXIT_FAILURE);
    }

    int n_users_files, n_wish_list;
    fscanf(fp, "%d", &n_users_files);
    initialize_users_files(n_users_files, fp);

    send_users_files_to_tracker(n_users_files);

    fscanf(fp, "%d", &n_wish_list);
    initialize_wish_list(n_wish_list, fp);

    fclose(fp);

    wait_for_tracker_confirmation();
    start_threads(rank, n_wish_list, number_of_tasks);
    free_allocated_memory();
}

 
int main (int argc, char *argv[]) {
    int number_of_tasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &number_of_tasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(number_of_tasks, rank);
    } else {
        gestionate_files(number_of_tasks, rank);
    }

    MPI_Finalize();

}