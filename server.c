#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>  // Para basename
#include <time.h>
#include <netdb.h>

#define PORT 8889

//prototypes
char *generateStateFileName(const char *directoryPath);
typedef struct {
    char filename[256];
    off_t size;
    time_t mod_time;
    char status[32];
} FileInfo;

typedef enum {
    NEW,
    MODIFIED,
    DELETED
} FileChangeType;

typedef struct {
    char filename[256];
    FileChangeType changeType;
} FileChange;

typedef struct {
    FileChange *changes;
    int count;
} FileChangeList;

void error(const char *msg) {
    perror(msg);
    exit(1);
}

void listFiles(int socket, const char *directoryPath) {
    DIR *d;
    struct dirent *dir;
    d = opendir(directoryPath);
    char buffer[1024];

    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_REG) { // Asegurar que es un archivo regular
                snprintf(buffer, sizeof(buffer), "%s\n", dir->d_name);
                write(socket, buffer, strlen(buffer));
            }
        }
        closedir(d);
    }
    write(socket, "end", strlen("end")); // Marca el fin de la lista de archivos
}

void processClientChanges(int socket, FileInfo *serverFiles, int serverFileCount) {
    char buffer[4096];
    int n;
    FileInfo clientFile;

    while (1) {
        bzero(buffer, sizeof(buffer));
        n = read(socket, buffer, sizeof(buffer) - 1);
        if (n < 0) error("ERROR reading from socket");
        if (n == 0) break; // Fin de la transmisión

        if (strncmp(buffer, "END_OF_CHANGES", 14) == 0) {
            break; // Fin de la recepción de cambios
        }

        sscanf(buffer, "File: %s\nSize: %ld\nLast Modified: %ld\nStatus: %s\n", 
               clientFile.filename, 
               &clientFile.size, 
               &clientFile.mod_time, 
               clientFile.status);

        int found = 0;
        for (int i = 0; i < serverFileCount; i++) {
            // printf("\n");
            // printf("\n");
            // printf(clientFile.filename); 
            // printf("\n");
            // printf(serverFiles[i].filename);
            // printf("\n");
            if (strcmp(clientFile.filename, serverFiles[i].filename) == 0) {
                found = 1;
                // Comparar el estado del archivo
                if (strcmp(clientFile.status, "eliminado") == 0 || strcmp(serverFiles[i].status, "eliminado") == 0) {
                    printf("Archivo %s eliminado\n", clientFile.filename);
                } else if (strcmp(clientFile.status, "modificado") == 0 || strcmp(serverFiles[i].status, "modificado") == 0) {
                    printf("Archivo %s modificado\n", clientFile.filename);
                } else {
                    printf("Archivo %s intacto\n", clientFile.filename);
                }
                break;
            }
        }
        if (!found) {
            printf("Archivo %s nuevo\n", clientFile.filename);
        }
    }
}

char *generateStateFileName(const char *directoryPath) {
    char *dirName = strdup(directoryPath);
    char *baseName = basename(dirName);
    char *stateFileName = malloc(strlen(baseName) + strlen("_server-state.txt") + 1);
    if (stateFileName == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    sprintf(stateFileName, "%s_server-state.txt", baseName);
    free(dirName);
    return stateFileName;
}

void readDirectory(const char *directoryPath, FileInfo **files, int *fileCount) {
    DIR *d;
    struct dirent *dir;
    d = opendir(directoryPath);
    if(!d) {
        perror("Error al abrir el directorio");
        exit(1);
    }

    *fileCount = 0;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG) { // Asegurar que es un archivo regular
            *fileCount += 1;
        }
    }

    *files = malloc(sizeof(FileInfo) * (*fileCount));
    if(!*files) {
        perror("Error al asignar memoria");
        exit(1);
    }

    rewinddir(d);
    int i = 0;
    struct stat fileInfo;

    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG) { // Asegurar que es un archivo regular
            snprintf((*files)[i].filename, sizeof((*files)[i].filename), "%s", dir->d_name);

            char filePath[1024];
            snprintf(filePath, sizeof(filePath), "%s/%s", directoryPath, dir->d_name);

            if (stat(filePath, &fileInfo) < 0) {
                perror("Error al obtener información del archivo");
                printf("No se pudo acceder a: %s\n", filePath); // Mostrar el error con la ruta del archivo
                continue;
            }

            (*files)[i].size = fileInfo.st_size;
            (*files)[i].mod_time = fileInfo.st_mtime;
            printf("Archivo: %s\n", (*files)[i].filename);
            i++;
        }
    }
    closedir(d);
}

void loadPreviousState(const char *stateFilePath, FileInfo **previousFiles, int *previousFileCount) {
    FILE *file = fopen(stateFilePath, "r");
    if (file == NULL) {
        printf("No se encontró el archivo de estado: %s\n", stateFilePath);
        *previousFiles = NULL;
        *previousFileCount = 0;
        return;
    }
    char line[1024];
    *previousFileCount = 0;
    while (fgets(line, sizeof(line), file)) {
        (*previousFileCount)++;
    }

    // Asignar memoria para los archivos previos
    *previousFiles = malloc(sizeof(FileInfo) * (*previousFileCount));
    if (!*previousFiles) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
 // Volver al inicio del archivo para leer los datos
    rewind(file);
    int idx = 0;
    while (fgets(line, sizeof(line), file)) {
        sscanf(line, "%s %ld %ld", 
               (*previousFiles)[idx].filename, 
               &(*previousFiles)[idx].size, 
               &(*previousFiles)[idx].mod_time);
        idx++;
    }

    fclose(file);
}

void startServer(const char *directoryPath, FileInfo *serverFiles, int serverFileCount) {
    int sockfd, newsockfd;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) 
       error("ERROR opening socket");

    bzero((char *) &serv_addr, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) 
            error("ERROR on binding");

    listen(sockfd, 5);
    clilen = sizeof(cli_addr);

    newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0) 
          error("ERROR on accept");

    processClientChanges(newsockfd, serverFiles, serverFileCount); // Procesar los cambios enviados por el cliente

    close(newsockfd);
    close(sockfd);
}


void writeStateFile(const char *stateFileName, const FileInfo *files, int fileCount) {
    FILE *file = fopen(stateFileName, "w");
    if (!file) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < fileCount; i++) {
        fprintf(file, "%s %lld %ld %s\n", 
                files[i].filename, 
                (long long)files[i].size, 
                (long)files[i].mod_time,
                files[i].status);
    }
    fclose(file);
}
void compareAndUpdateFileStates(FileInfo **currentFiles, int *currentFileCount, FileInfo *previousFiles, int previousFileCount) {
    int newCurrentFileCount = *currentFileCount + previousFileCount; // Máximo posible si todos los archivos anteriores fueron eliminados
    FileInfo *newCurrentFiles = (FileInfo *)realloc(*currentFiles, sizeof(FileInfo) * newCurrentFileCount);
    if (!newCurrentFiles) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    *currentFiles = newCurrentFiles;

    for (int i = 0; i < *currentFileCount; i++) {
        int found = 0;
        for (int j = 0; j < previousFileCount; j++) {
            if (strcmp((*currentFiles)[i].filename, previousFiles[j].filename) == 0) {
                found = 1;
                if ((*currentFiles)[i].mod_time != previousFiles[j].mod_time) {
                    strcpy((*currentFiles)[i].status, "modificado");
                } else {
                    strcpy((*currentFiles)[i].status, "intacto");
                }
                break;
            }
        }
        if (!found) {
            strcpy((*currentFiles)[i].status, "nuevo");
        }
    }

    int currentIndex = *currentFileCount;
    for (int j = 0; j < previousFileCount; j++) {
        int found = 0;
        for (int i = 0; i < *currentFileCount; i++) {
            if (strcmp(previousFiles[j].filename, (*currentFiles)[i].filename) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            // Agrega el archivo eliminado a la lista actual con estado "eliminado"
            strcpy((*currentFiles)[currentIndex].filename, previousFiles[j].filename);
            (*currentFiles)[currentIndex].size = previousFiles[j].size;
            (*currentFiles)[currentIndex].mod_time = previousFiles[j].mod_time;
            strcpy((*currentFiles)[currentIndex].status, "eliminado");
            currentIndex++;
        }
    }
    *currentFileCount = currentIndex; // Actualizar el contador de archivos actuales
}
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,"Uso: %s <directorio>\n", argv[0]);
        exit(1);
    }

    char *stateFileName = generateStateFileName(argv[1]);
    printf("Nombre del archivo de estado: %s\n", stateFileName);
    printf("Directorio a monitorear: %s\n", argv[1]);

    FileInfo *currentFiles, *previousFiles;
    int currentFileCount, previousFileCount;
    
    readDirectory(argv[1], &currentFiles, &currentFileCount);
    loadPreviousState(stateFileName, &previousFiles, &previousFileCount);
    compareAndUpdateFileStates(&currentFiles, &currentFileCount, previousFiles, previousFileCount);
    writeStateFile(stateFileName, currentFiles, currentFileCount);

    for (int i = 0; i < currentFileCount; i++) {
        printf("Archivo: %s\n", currentFiles[i].filename);
        printf("Tamaño: %ld bytes\n", currentFiles[i].size);
        printf("Última modificación: %s\n", ctime(&currentFiles[i].mod_time));
    }

    free(currentFiles);
    if (previousFiles != NULL) {
        free(previousFiles);
    }
    free(stateFileName);

    // Nueva parte: leer el directorio actual del servidor
    FileInfo *serverFiles;
    int serverFileCount;
    readDirectory(argv[1], &serverFiles, &serverFileCount);

    // Iniciar servidor
    startServer(argv[1], serverFiles, serverFileCount);

    // Liberar memoria de serverFiles
    free(serverFiles);

    return 0;
}
