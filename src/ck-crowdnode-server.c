/*
# ck-crowdnode
#
# Standalone, thin and portable server to let users participate in experiment crowdsourcing via CK
#
# See LICENSE.txt for licensing details.
# See Copyright.txt for copyright details.
#
# Developer: Daniil Efremov
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(__linux__) || defined(__APPLE__)
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <sys/socket.h> /* socket, connect */
    #include <netdb.h> /* struct hostent, gethostbyname */
    #include <netinet/in.h> /* struct sockaddr_in, struct sockaddr */
    #include <ctype.h>

#elif _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>

    struct thread_win_params {
      int sock;
      int newsock;
      char * baseDir;
    };

    void doProcessingWin (struct thread_win_params* twp);

    #pragma comment(lib,"ws2_32.lib") //Winsock Library

#else
#endif

#include "cJSON.h"
#include "base64.h"
#include "urldecoder.h"
#include "net_uuid.h"

static char *const CK_JSON_KEY = "ck_json=";

static char *const JSON_PARAM_NAME_COMMAND = "action";
static char *const JSON_PARAM_PARAMS = "parameters";
static char *const JSCON_PARAM_VALUE_PUSH = "push";
static char *const JSON_PARAM_FILE_NAME = "filename";
static char *const JSON_PARAM_FILE_CONTENT = "file_content_base64";
static char *const JSON_PARAM_SHELL_COMMAND = "cmd";

/**
 * todo move out to config /etc/ck-crowdnode/ck-crowdnode.properties
 */
#define MAX_BUFFER_SIZE 1024
static const int DEFAULT_SERVER_PORT = 3333;
static const int MAXPENDING = 5;    /* Maximum outstanding connection requests */

static char *const JSON_CONFIG_PARAM_PORT = "port";
static char *const JSON_CONFIG_PARAM_PATH_TO_FILES = "path_to_files";
static char *const JSON_CONFIG_PARAM_SECRET_KEY = "secret_key";

#ifdef _WIN32
static char *const DEFAULT_BASE_DIR = "C:\\tmp\\"; //todo move to config
static char *const DEFAULT_CONFIG_FILE_PATH = "%LOCALAPPDATA%/.ck-crowdnode/ck-crowdnode-config.json";
static char *const HOME_DIR_TEMPLATE = "%LOCALAPPDATA%";
static char *const HOME_DIR_ENV_KEY = "LOCALAPPDATA";
#else
static char *const DEFAULT_BASE_DIR = "/tmp/";
static char *const DEFAULT_CONFIG_FILE_PATH = "$HOME/.ck-crowdnode/ck-crowdnode-config.json";
static char *const HOME_DIR_TEMPLATE = "$HOME";
static char *const HOME_DIR_ENV_KEY = "HOME";

int WSAGetLastError() {
	return 0;
}
#endif

/**
 * Input: command in CK JSON format TDB
 * Output: Execution result in CK JSON format
 *
 * Examples:
 * push command
 *   input JSON:
 *     {"command":"push", "parameters": {"filename":"file1", "data":"<base64 encoded binary file data >"} }
 *
 *   output result JSON:
 *     {"state":"finished", "compileUUID":"567567567567567"}
 *
 * run command
 *   input JSON:
 *     {"command":"run", "parameters":{"compileUUID":"567567567567567"} }
 *
 *   output result JSON:
 *     {"state":"in progress", "runUUID":"12312312323213"}
 *     {"state":"finished ok"}
 *     {"state":"finished error", "errorMessage":"File not found"}
 *
 * state command
 *   input JSON:
 *     {"command":"state", "parameters":{"runUUID":"12312312323213"} }
 *
 *   output result JSON:
 *     {"state":"in progress"}
 *     {"state":"finished ok"}
 *     {"state":"finished error", "errorMessage":"File not found"}
 *
 * pull command
 *   input JSON:
 *     {"command":"pull", "parameters":{"runUUID":"12312312323213"}}
 *
 *   output result JSON:
 *     {"state":"finished", "parameters": {"filename":"file1", "data":"<base64 encoded binary file data >"} }
 *
 *
 * todo list:
 * 2) Check/Implement concurrent execution - looks like thread fors well at linus and windows as well
 * 3) Implement "shell' commnad
 */

void doProcessing(int sock, char *baseDir);

void sendErrorMessage(int sock, char * errorMessage, const char *errorCode) {
	perror(errorMessage);

	cJSON *resultJSON = cJSON_CreateObject();
    if (!resultJSON) {
        perror("[ERROR]: resultJSON cannot be created");
        return;
    }

    cJSON_AddItemToObject(resultJSON, "return", cJSON_CreateString(errorCode));
	cJSON_AddItemToObject(resultJSON, "error", cJSON_CreateString(errorMessage));
	char *resultJSONtext = cJSON_Print(resultJSON);
    if (!resultJSONtext) {
        perror("[ERROR]: resultJSONtext cannot be created");
        return;
    }
#ifdef _WIN32
    int n =send( sock, resultJSONtext, strlen(resultJSONtext), 0 );
#else
    int n = write(sock, resultJSONtext, strlen(resultJSONtext));
#endif

    if (n < 0) {
		perror("ERROR writing to socket");
		return ;
	}
    free(resultJSONtext);
    cJSON_Delete(resultJSON);
}

char* concat(const char *str1, const char *str2) {
    size_t totalSize = strlen(str1) + strlen(str2) + sizeof(char);
    char *message = malloc(totalSize);
    memset(message, 0, totalSize);

    if(!message){
        printf("[ERROR]: Memory not allocated for concat\n");
        exit(-1);
    }

    strcat(message, str1);
    strcat(message + strlen(str1), str2);
    return message;
}

void dieWithError(char *error) {
    printf("Connection error: %s %i", error, WSAGetLastError());
    exit(1);
}

typedef struct {
    int	port;
    char *pathToFiles;
    char *secretKey;

} CKCrowdnodeServerConfig;

CKCrowdnodeServerConfig *ckCrowdnodeServerConfig;
char *serverSecretKey;


static char *const JSON_PARAM_NAME_SECRETKEY = "secretkey";
static char *const ERROR_MESSAGE_SECRET_KEY_MISSMATCH = "secret keys do not match";
static char *const ERROR_CODE_SECRET_KEY_MISMATCH = "3";
static char *const ERROR_CODE = "1";

char *str_replace(char *orig, char *rep, char *with) {
    char *result; // the return string
    char *ins;    // the next insert point
    char *tmp;    // varies
    int len_rep;  // length of rep
    int len_with; // length of with
    int len_front; // distance between rep and end of last rep
    int count;    // number of replacements

    if (!orig)
        return NULL;
    if (!rep)
        rep = "";
    len_rep = strlen(rep);
    if (!with)
        with = "";
    len_with = strlen(with);

    ins = orig;
    for (count = 0; tmp = strstr(ins, rep); ++count) {
        ins = tmp + len_rep;
    }

    // first time through the loop, all the variable are set correctly
    // from here on,
    //    tmp points to the end of the result string
    //    ins points to the next occurrence of rep in orig
    //    orig points to the remainder of orig after "end of rep"
    tmp = result = malloc(strlen(orig) + (len_with - len_rep) * count + 1);

    if (!result)
        return NULL;

    while (count--) {
        ins = strstr(orig, rep);
        len_front = ins - orig;
        tmp = strncpy(tmp, orig, len_front) + len_front;
        tmp = strcpy(tmp, with) + len_with;
        orig += len_front + len_rep; // move to next "end of rep"
    }
    strcpy(tmp, orig);
    return result;
}

char *getEnvValue(char *param, char** envp ) {
    char * value;
    while (*envp) {
        if (strstr(*envp, param) != NULL) {
            value = malloc(strlen(*envp) + 1);
            if (!value) {
                perror("[ERROR]: Memory not allocated for getEnvValue");
            }
            strcpy(value, *envp);
            char *rep = concat(param, "=");
            char *string = str_replace(value, rep, "");
            return string;
        }
        *envp++;
    }
    return NULL;
}


char* getAbsolutePath(char *pathToFiles, char** envp) {
    size_t size = strlen(pathToFiles) + sizeof(char);
    char * absolutePath = malloc(size);
    memset(absolutePath, 0, size);
    strcpy(absolutePath, pathToFiles);
    if (strstr(absolutePath, HOME_DIR_TEMPLATE) != NULL) {
        return str_replace(absolutePath, HOME_DIR_TEMPLATE, getEnvValue(HOME_DIR_ENV_KEY, envp));
    }
    return absolutePath;
}

int loadConfigFromFile(CKCrowdnodeServerConfig *ckCrowdnodeServerConfig, char** envp) {
    char *filePath = getAbsolutePath(DEFAULT_CONFIG_FILE_PATH, envp);

    FILE *file=fopen(filePath, "rb");
    if (!file) {
        printf("[ERROR]: File not found at path: %s\n", filePath);
        return 0;
    }

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *fileContent = malloc(fsize + 1);
    memset(fileContent,0, fsize + 1);
    fread(fileContent, fsize, 1, file);
    fclose(file);

    cJSON *configSON = cJSON_Parse(fileContent);
    if (!configSON) {
        printf("[ERROR]: Invalid JSON format for configuration file %s\n", filePath);
        return 0;
    }

    cJSON *portJSON= cJSON_GetObjectItem(configSON, JSON_CONFIG_PARAM_PORT);
    if (!portJSON) {
        printf("[ERROR]: Invalid JSON format for provided message, attribute %s not found\n", JSON_CONFIG_PARAM_PORT);
        if (configSON != NULL) {
            cJSON_Delete(configSON);
        }
        return 0;
    }
    int port = portJSON->valueint;
    ckCrowdnodeServerConfig->port =port;

    cJSON *pathSON = cJSON_GetObjectItem(configSON, JSON_CONFIG_PARAM_PATH_TO_FILES);
    if (!pathSON) {
        printf("[ERROR]: Invalid JSON format for provided message, attribute %s not found\n", JSON_CONFIG_PARAM_PATH_TO_FILES);
        if (configSON != NULL) {
            cJSON_Delete(configSON);
        }
        return 0;
    }
    char *pathToFiles = getAbsolutePath(pathSON->valuestring, envp);
    ckCrowdnodeServerConfig->pathToFiles = concat(pathToFiles, "/");

    char * secretKey;
    cJSON *secretKeyJSON = cJSON_GetObjectItem(configSON, JSON_CONFIG_PARAM_SECRET_KEY);
    if (!secretKeyJSON) {
        printf("[ERROR]: Invalid JSON format for provided message, attribute %s not found\n", JSON_CONFIG_PARAM_SECRET_KEY);
        if (configSON != NULL) {
            cJSON_Delete(configSON);
        }
        return 0;
    } else {
        secretKey = secretKeyJSON->valuestring;
    }
    size_t size = strlen(secretKey) + sizeof(char);
    ckCrowdnodeServerConfig->secretKey = malloc(size);
    memset(ckCrowdnodeServerConfig->secretKey, 0, size);
    strcpy(ckCrowdnodeServerConfig->secretKey, secretKey);
    cJSON_Delete(configSON);
    return 1;
}

int loadDefaultConfig(CKCrowdnodeServerConfig *ckCrowdnodeServerConfig, char** envp) {
    ckCrowdnodeServerConfig->port = DEFAULT_SERVER_PORT;
    ckCrowdnodeServerConfig->pathToFiles = getAbsolutePath(DEFAULT_BASE_DIR, envp);
    char generatedSecretKey[38];
    get_uuid_string(generatedSecretKey, sizeof(generatedSecretKey));
    size_t size = strlen(generatedSecretKey) + sizeof(char);
    ckCrowdnodeServerConfig->secretKey = malloc(size);
    memset(ckCrowdnodeServerConfig->secretKey, 0, size);
    strcpy(ckCrowdnodeServerConfig->secretKey, generatedSecretKey);
}

int main( int argc, char *argv[] , char** envp) {

    printf("[INFO]: CK-crowdnode-server starting ...\n");
    printf("[INFO]: %s env value: %s\n", HOME_DIR_TEMPLATE, getEnvValue(HOME_DIR_ENV_KEY, envp));
    printf("[INFO]: Configuration file absolute path: %s\n", getAbsolutePath(DEFAULT_CONFIG_FILE_PATH, envp));
    ckCrowdnodeServerConfig = malloc(sizeof(CKCrowdnodeServerConfig));
    if (!ckCrowdnodeServerConfig) {
        perror("[ERROR]: Memory not allocated for ckCrowdnodeServerConfig\n");
        exit(1);
    }

    if (!loadConfigFromFile(ckCrowdnodeServerConfig, envp)) {
        loadDefaultConfig(ckCrowdnodeServerConfig, envp);
        printf("[WARN]: CK-crowdnode-server configuration file problem. Server will be started with default configuration, port: %i, pathToFiles: %s, secret_key: %s\n",
               ckCrowdnodeServerConfig->port,
               ckCrowdnodeServerConfig->pathToFiles,
               ckCrowdnodeServerConfig->secretKey
        );
        ckCrowdnodeServerConfig->port = DEFAULT_SERVER_PORT;
        ckCrowdnodeServerConfig->pathToFiles = DEFAULT_BASE_DIR;
    } else {
        printf("[INFO]: CK-crowdnode-server configuration file loaded successfully with configuration, port: %i, pathToFiles: %s, secret_key: %s\n",
               ckCrowdnodeServerConfig->port,
               ckCrowdnodeServerConfig->pathToFiles,
               ckCrowdnodeServerConfig->secretKey
        );
    }

    serverSecretKey = ckCrowdnodeServerConfig->secretKey;
    int sockfd, newsockfd;
	socklen_t clilen;
    int portno = ckCrowdnodeServerConfig->port;
	char *baseDir = malloc(strlen(ckCrowdnodeServerConfig->pathToFiles) * sizeof(char) + 1);
    if (!baseDir) {
        perror("Could not allocate memory for baseDir");
    }
    strcpy(baseDir, ckCrowdnodeServerConfig->pathToFiles);
	unsigned long win_thread_id;

#ifdef _WIN32
	struct thread_win_params twp;
	struct thread_win_params* ptwp=&twp;
#endif

	struct sockaddr_in serv_addr, cli_addr;

#ifdef _WIN32
    int servSock;                    /* Socket descriptor for server */
    int clntSock;                    /* Socket descriptor for client */
    struct sockaddr_in echoServAddr; /* Local address */
    struct sockaddr_in echoClntAddr; /* Client address */
    unsigned short echoServPort;     /* Server port */
    unsigned int clntLen;            /* Length of client address data structure */
    WSADATA wsaData;                 /* Structure for WinSock setup communication */

    echoServPort = DEFAULT_SERVER_PORT;

    if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0) /* Load Winsock 2.0 DLL */
    {
        fprintf(stderr, "WSAStartup() failed");
        exit(1);
    }

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        dieWithError("socket() failed");
    }

    /* Construct local address structure */
    memset(&echoServAddr, 0, sizeof(echoServAddr));   /* Zero out structure */
    echoServAddr.sin_family = AF_INET;                /* Internet address family */
    echoServAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    echoServAddr.sin_port = htons(echoServPort);      /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *) &echoServAddr, sizeof(echoServAddr)) < 0) {
        dieWithError("bind() failed");
    }

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) < 0) {
        dieWithError("listen() failed");
    }

#else
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

	if (sockfd < 0) {
		perror("ERROR opening socket");
        printf("WSAGetLastError() %i\n", WSAGetLastError()); //win
		exit(1);
	}

	memset((char *) &serv_addr, 0, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);

	if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		perror("ERROR on binding");
		exit(1);
	}
	printf("[INFO]: Server started at port  %i\n", portno);

	listen(sockfd,5);
	clilen = sizeof(cli_addr);
#endif


	/**
     * Main server loop
     */
	while (1) {

		/**
         * Create child process
         */
#ifdef _WIN32
        /* Set the size of the in-out parameter */
        clntLen = sizeof(echoClntAddr);

        /* Wait for a client to connect */
        printf("[INFO] CK-crowdnode-server listen commands on port %i\n", portno);
        if ((clntSock = accept(servSock, (struct sockaddr *) &echoClntAddr, &clntLen)) < 0) {
            dieWithError("accept() failed");
        }

        ptwp->sock=servSock;
		ptwp->newsock=clntSock;
        ptwp->baseDir=baseDir;

		if (!CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)doProcessingWin,
						  (struct thread_win_params*) ptwp, 0, &win_thread_id))
		{
			perror("ERROR on fork");
			exit(1);
		}

/*		closesocket(sockfd); */
#else

        newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);

		if (newsockfd < 0) {
			perror("ERROR on accept");
            printf("WSAGetLastError() %i\n", WSAGetLastError()); //win
			exit(1);
		}
		pid_t pid = fork();

        if (pid < 0) {
            perror("ERROR on fork");
            exit(1);
        }

        if (pid == 0) {
            close(sockfd);
            doProcessing(newsockfd, baseDir);
            exit(0);
        } else {
            close(newsockfd);
        }
#endif
	}
}

#ifdef _WIN32
void doProcessingWin (struct thread_win_params* ptwp)
{
	int sockfd=ptwp->sock;
	int newsockfd=ptwp->newsock;
	char *baseDir = ptwp->baseDir;

	// Child process - talk with connected client
	doProcessing (newsockfd, baseDir);

	if (shutdown (newsockfd, 2)!=0)
	{
		perror("Error on fork");
		exit(1);
	}

	closesocket(newsockfd);

	return;
}
#endif



void doProcessing(int sock, char *baseDir) {
    char *client_message = malloc(MAX_BUFFER_SIZE + 1);
    if (client_message == NULL) {
        perror("[ERROR]: Memory not allocated for client_message first time");
        exit(1);
    }

    char *buffer = malloc(MAX_BUFFER_SIZE + 1);
    if (buffer == NULL) {
        perror("[ERROR]: Memory not allocated buffer");
        exit(1);
    }

    memset(buffer, 0, MAX_BUFFER_SIZE);
    int buffer_read = 0;
    int total_read = 0;

    //buffered read from socket
    int i = 0;
    while(1) {
        buffer_read = recv(sock, buffer, MAX_BUFFER_SIZE, 0);
        if (buffer_read > 0) {
            client_message = realloc(client_message, total_read + buffer_read + 1);
            if (client_message == NULL) {
                perror("Error ! Memory not allocated client_message");
                exit(1);
            }
            buffer[buffer_read] = '\0';
            memcpy(client_message + total_read, buffer, buffer_read);
            total_read = total_read + buffer_read;
            printf("Next %i part of buffer\n", i);
            i++;
        } else if (buffer_read < 0) {
            perror("[ERROR]: reading from socket");
            printf("WSAGetLastError() %i\n", WSAGetLastError()); //win
            exit(1);
        }
        if (buffer_read == 0 || buffer_read < MAX_BUFFER_SIZE) {
            /* message received successfully */
            break;
        }
    }
    if (buffer == NULL) {
        perror("Error ! Try to free not allocated memory buffer");
        exit(1);
    }
    free(buffer);
    client_message[total_read] = '\0';
    printf("[DEBUG]: Post request length: %lu\n", (unsigned long) strlen(client_message));

	char *decodedJSON;
	char *encodedJSONPostData = strstr(client_message, CK_JSON_KEY);
	if (encodedJSONPostData != NULL) {
		char *encodedJSON = encodedJSONPostData + strlen(CK_JSON_KEY);
		decodedJSON = url_decode(encodedJSON, total_read - (encodedJSON - client_message));
	} else {
		decodedJSON = client_message;
	}

	cJSON *commandJSON = cJSON_Parse(decodedJSON);
	if (!commandJSON) {
		sendErrorMessage(sock, "Invalid action JSON format for message", ERROR_CODE);
		return;
	}


    cJSON *secretkeyJSON = cJSON_GetObjectItem(commandJSON, JSON_PARAM_NAME_SECRETKEY);
    if (!secretkeyJSON) {
        if (commandJSON != NULL) {
            cJSON_Delete(commandJSON);
        }
        sendErrorMessage(sock, ERROR_MESSAGE_SECRET_KEY_MISSMATCH, ERROR_CODE_SECRET_KEY_MISMATCH);
        return;
    }
    char *clientSecretKey = secretkeyJSON->valuestring;
    printf("[ERROR]: Get secretkey: %sfrom client\n", clientSecretKey);
    if (!serverSecretKey || strncmp(clientSecretKey, serverSecretKey, strlen(serverSecretKey)) == 0 ) {


        cJSON *actionJSON = cJSON_GetObjectItem(commandJSON, JSON_PARAM_NAME_COMMAND);
        if (!actionJSON) {
            printf("[ERROR]: Invalid action JSON format for message: \n");
            if (commandJSON != NULL) {
                cJSON_Delete(commandJSON);
            }
            sendErrorMessage(sock, "Invalid action JSON format for message: no action found", ERROR_CODE);
            return;
        }
        char *action = actionJSON->valuestring;

        printf("[INFO]: Get action: %s\n", action);
        char *resultJSONtext;
        if (strncmp(action, JSCON_PARAM_VALUE_PUSH, 4) == 0) {
            //  push file (to send file to CK Node )
            cJSON *filenameJSON = cJSON_GetObjectItem(commandJSON, JSON_PARAM_FILE_NAME);
            if (!filenameJSON) {
                printf("[ERROR]: Invalid action JSON format for provided message\n");
                if (commandJSON != NULL) {
                    cJSON_Delete(commandJSON);
                }
                sendErrorMessage(sock, "Invalid action JSON format for message: no filenameJSON found", ERROR_CODE);
                return;
            }

            char *fileName = filenameJSON->valuestring;
            cJSON *fileContentJSON = cJSON_GetObjectItem(commandJSON, JSON_PARAM_FILE_CONTENT);
            if (!fileContentJSON) {
                printf("[ERROR]: Invalid action JSON format for message: \n");
                if (commandJSON != NULL) {
                    cJSON_Delete(commandJSON);
                }
                sendErrorMessage(sock, "Invalid action JSON format for message: no fileContentJSON found", ERROR_CODE);
                return;
            }

            char *file_content_base64 = fileContentJSON->valuestring;

            printf("[DEBUG]: File name: %s\n", fileName);
            printf("[DEBUG]: File content base64 length: %lu\n", (unsigned long) strlen(file_content_base64));

            int targetSize = ((unsigned long) strlen(file_content_base64) + 1) * 4 / 3;
            unsigned char *file_content = malloc(targetSize);

            int bytesDecoded = 0;
            if (strlen(file_content_base64) != 0) {
                bytesDecoded = base64_decode(file_content_base64, file_content, targetSize);
                if (bytesDecoded == 0) {
                    sendErrorMessage(sock, "Failed to Base64 decode file", ERROR_CODE);
                }
                file_content[bytesDecoded] = '\0';
                printf("[INFO]: Bytes decoded: %i\n", bytesDecoded);
            } else {
                printf("[WARNING]: file content is empty nothing to decode\n");
            }

            // 2) save locally at tmp dir
            printf("[DEBUG]: Build file path from base dir: %s and file name: %s\n", baseDir, fileName);
            char *filePath = concat(baseDir, fileName);

            FILE *file = fopen(filePath, "wb");
            if (!file) {
                char *message = concat("Could not write file at path: ", filePath);
                printf("[ERROR]: %s", message);
                if (commandJSON != NULL) {
                    cJSON_Delete(commandJSON);
                }
                sendErrorMessage(sock, message, ERROR_CODE);
                return;
            }

            printf("[DEBUG]: Open file to write %s\n", filePath);
            printf("[DEBUG]: Bytes to write %i\n", bytesDecoded);
            int results = fwrite(file_content, 1, bytesDecoded, file);
            if (results == EOF) {
                sendErrorMessage(sock, "Failed to write file ", ERROR_CODE);
            }
            fclose(file);
            free(file_content);
            printf("[INFO]: File saved to: %s\n", filePath);

            /**
             * return successful response message, example:
             *   {"return":0, "compileUUID": <generated UID>}
             */
            char compileUUID[38];
            get_uuid_string(compileUUID, sizeof(compileUUID));

            cJSON *resultJSON = cJSON_CreateObject();
            if (!resultJSON) {
                perror("[ERROR]: Memory not allocated for resultJSON");
                exit(1);
            }
            printf("[INFO]: resultJSON created\n");
            cJSON_AddItemToObject(resultJSON, "return", cJSON_CreateString("0"));
            cJSON_AddItemToObject(resultJSON, "compileUUID", cJSON_CreateString(compileUUID));
            resultJSONtext = cJSON_Print(resultJSON);
            cJSON_Delete(resultJSON);
        } else if (strncmp(action, "pull", 4) == 0) {
            //  pull file (to receive file from CK node)
            cJSON *filenameJSON = cJSON_GetObjectItem(commandJSON, JSON_PARAM_FILE_NAME);
            if (!filenameJSON) {
                printf("[ERROR]: Invalid action JSON format for provided message\n");
                //todo check if need to cJSON_Delete(commandJSON) here as well
                sendErrorMessage(sock, "Invalid action JSON format for message: no filenameJSON found", ERROR_CODE);
                return;
            }

            char *fileName = filenameJSON->valuestring;
            char *filePath = concat(baseDir, fileName);
            printf("[DEBUG]: Reading file: %s\n", filePath);
            FILE *file = fopen(filePath, "rb");
            if (!file) {
                char *message = concat("File not found at path:", filePath);
                printf("[ERROR]: %s", message);

                if (commandJSON != NULL) {
                    cJSON_Delete(commandJSON);
                }

                sendErrorMessage(sock, message, ERROR_CODE);
                return;
            }

            fseek(file, 0, SEEK_END);
            long fsize = ftell(file);
            fseek(file, 0, SEEK_SET);

            char *fileContent = malloc(fsize + 1);
            memset(fileContent, 0, fsize + 1);
            fread(fileContent, fsize, 1, file);
            fclose(file);

            fileContent[fsize] = 0;
            printf("[DEBUG]: File size: %lu\n", fsize);

            unsigned long targetSize = (unsigned long) ((fsize) * 4 / 3 + 5);
            printf("[DEBUG]: Target encoded size: %lu\n", targetSize);
            char *encodedContent = malloc(targetSize);
            if (!encodedContent) {
                perror("[ERROR]: Memory not allocated for encodedContent");
                exit(1);
            }

            if (fsize > 0) {
                base64_encode(fileContent, fsize, encodedContent, targetSize);
            }

            /**
             * return successful response message, example:
             *   {"return":0, "filename": <file name from requies>, "file_content_base64":<base 64 encoded requested file content>}
             */
            cJSON *resultJSON = cJSON_CreateObject();
            if (!resultJSON) {
                perror("[ERROR]: Memory not allocated for resultJSON");
                exit(1);
            }
            cJSON_AddItemToObject(resultJSON, "return", cJSON_CreateString("0"));
            cJSON_AddItemToObject(resultJSON, JSON_PARAM_FILE_NAME, cJSON_CreateString(fileName));
            cJSON_AddItemToObject(resultJSON, JSON_PARAM_FILE_CONTENT, cJSON_CreateString(encodedContent));
            resultJSONtext = cJSON_Print(resultJSON);
            cJSON_Delete(resultJSON);
        } else if (strncmp(action, "shell", 4) == 0) {
            //  shell (to execute a binary at CK node)
            // todo implement:
            // 1) find local file by provided name - send JSON error if not found
            // 2) generate run ID
            // 3) fork new process for async execute
            // 3) return run UUID as JSON sync with run UID and send to client
            // 4) in async process convert to JSON with ru UID and send to client
            printf("[DEBUG]: Request for shell command %s\n", decodedJSON);

            cJSON *shellCommandJSON = cJSON_GetObjectItem(commandJSON, JSON_PARAM_SHELL_COMMAND);
            if (!shellCommandJSON) {
                printf("[ERROR]: Invalid action JSON format for provided message\n");
                //todo check if need to cJSON_Delete(commandJSON) here as well
                sendErrorMessage(sock, "Invalid action JSON format for message: no filenameJSON found", ERROR_CODE);
                return;
            }

            char *shellCommand = shellCommandJSON->valuestring;

            if (!shellCommand) {
                printf("[ERROR]: Invalid action JSON format for provided message\n");
                //todo check if need to cJSON_Delete(commandJSON) here as well
                sendErrorMessage(sock, "Invalid action JSON format for message: no filenameJSON found", ERROR_CODE);
                return;
            }

            int systemReturnCode = system(shellCommand);

            char path[MAX_BUFFER_SIZE + 1];
            char *stdoutText = malloc(MAX_BUFFER_SIZE + 1);
            if (stdoutText == NULL) {
                perror("[ERROR]: Memory not allocated for stdoutText first time");
                exit(1);
            }
            memset(stdoutText, 0, MAX_BUFFER_SIZE + 1);

            /* Open the command for reading. */
            FILE *fp;
#ifdef _WIN32
            fp = _popen(shellCommand, "r");
#else
            fp = popen(shellCommand, "r");
#endif
            if (fp == NULL) {
                printf("[ERROR]: Failed to run command: %s\n", shellCommand);
                exit(1);
            }

            int buffer_read = 0;
            int total_read = 0;
            while (fgets(path, sizeof(path) - 1, fp) != NULL) {
                buffer_read = sizeof(path) - 1;
                printf("[INFO]: Next buffer_read: %i\n", buffer_read);
                printf("[INFO]: Next stdout line length: %lu, line text: %s", (unsigned long)(strlen(path)), path);
                stdoutText = realloc(stdoutText, total_read + buffer_read + 1);
                if (stdoutText == NULL) {
                    perror("[ERROR]: Memory not allocated stdout");
                    exit(1);
                }
                strcat(stdoutText + strlen(stdoutText), path);
                total_read = total_read + buffer_read;
            }

#ifdef _WIN32
            _pclose(fp);
#else
            pclose(fp);
#endif

            printf("[INFO]: total stdout line length: %i\n", total_read);
            printf("[DEBUG]: stdout: %s\n", stdoutText);

            cJSON *resultJSON = cJSON_CreateObject();
            cJSON_AddItemToObject(resultJSON, "return", cJSON_CreateString("0"));

            cJSON_AddNumberToObject(resultJSON, "return_code", systemReturnCode);

            cJSON_AddItemToObject(resultJSON, "stdout", cJSON_CreateString(stdoutText)); 
            cJSON_AddItemToObject(resultJSON, "stderr", cJSON_CreateString("some program stderr"));   //todo get stderr
            resultJSONtext = cJSON_Print(resultJSON);
            cJSON_Delete(resultJSON);
        } else if (strncmp(action, "state", 4) == 0) {
            printf("[DEBUG]: Check run state by runUUID ");
            cJSON *params = cJSON_GetObjectItem(commandJSON, JSON_PARAM_PARAMS);
            char *runUUID = cJSON_GetObjectItem(params, "runUUID")->valuestring;
            printf("[DEBUG]: runUUID: %s\n", runUUID);

            //todo implement get actual runing state by runUUID

            cJSON *resultJSON = cJSON_CreateObject();
            cJSON_AddItemToObject(resultJSON, "return", cJSON_CreateString("0"));
            resultJSONtext = cJSON_Print(resultJSON);
            cJSON_Delete(resultJSON);
        } else if (strncmp(action, "clear", 4) == 0) {
            printf("[DEBUG]: Clearing tmp files ...");
            // todo implement removing all temporary files saved localy but need check some process could be in running state
            // so need to discus how it should work
        } else if (strncmp(action, "shutdown", 4) == 0) {
            printf("[DEBUG]: Start shutdown CK node");
            cJSON_Delete(commandJSON);
            return;
        } else {
            sendErrorMessage(sock, "unknown action", ERROR_CODE);
        }

#ifdef _WIN32
        int n1 =send( sock, resultJSONtext, strlen(resultJSONtext), 0 );
#else
        int n1 = write(sock, resultJSONtext, strlen(resultJSONtext));
#endif

        free(resultJSONtext);

        if (n1 < 0) {
            perror("ERROR writing to socket");
            return;
        }
    } else {
        sendErrorMessage(sock, ERROR_MESSAGE_SECRET_KEY_MISSMATCH, ERROR_CODE_SECRET_KEY_MISMATCH);
        return;
    }
	cJSON_Delete(commandJSON);
    if (client_message == NULL) {
        perror("Error ! Try to free not allocated memory client_message");
        exit(1);
    }
    free(client_message);

	printf("[INFO]: Action completed successfuly\n");
}

