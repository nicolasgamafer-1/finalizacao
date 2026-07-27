/*
 * server.c - Servidor de chat da Padaria
 * ---------------------------------------
 * Serve o arquivo chat.html e styles.css, e expoe duas rotas de API:
 *   GET  /api/mensagens   -> devolve o conteudo do arquivo compartilhado (chat.txt)
 *   POST /api/enviar      -> recebe "nome" e "mensagem" e acrescenta uma linha no chat.txt
 *
 * O arquivo chat.txt e o "bloco de notas" compartilhado: ele deve estar numa
 * pasta compartilhada da rede (ex: um drive mapeado \\PC-DA-COZINHA\chat\chat.txt).
 * Cada pessoa roda esse mesmo server.c na sua propria maquina, todos apontando
 * para o MESMO chat.txt. Assim, quando alguem envia uma mensagem, o arquivo
 * compartilhado muda, e o servidor de cada um le a mudanca e mostra na tela.
 *
 * COMO CONFIGURAR:
 *   1) Edite a constante CHAT_FILE_PATH la embaixo para apontar pro caminho
 *      real do chat.txt na pasta compartilhada da rede.
 *   2) Coloque chat.html e styles.css na MESMA pasta onde esta o server.exe.
 *
 * COMO COMPILAR NO WINDOWS (com MinGW/gcc):
 *   gcc server.c -o server.exe -lws2_32
 *
 * COMO RODAR:
 *   server.exe
 *   Depois abra no navegador: http://localhost:8080/chat.html
 *   (outras pessoas na mesma rede tambem podem acessar pelo IP da sua maquina,
 *    ex: http://192.168.0.10:8080/chat.html -- mas o mais simples e cada um
 *    rodar o proprio server.exe apontando pro chat.txt compartilhado)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---------- Caminho do arquivo compartilhado (edite aqui) ---------- */
#define CHAT_FILE_PATH   "chat.txt"
/* Exemplo de caminho de rede no Windows:
   #define CHAT_FILE_PATH   "Z:\\chat\\chat.txt"
   ou
   #define CHAT_FILE_PATH   "\\\\PC-DA-COZINHA\\compartilhado\\chat.txt"
*/

#define PORTA            8080
#define TAM_BUFFER       65536

/* ---------- Camada de rede: Windows (Winsock) x Linux/Mac (POSIX) ---------- */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define FECHAR_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    typedef int socket_t;
    #define FECHAR_SOCKET close
#endif

/* ---------- Utilidades ---------- */

/* Decodifica texto vindo de um formulario (application/x-www-form-urlencoded) */
static void url_decode(const char *entrada, char *saida, size_t tam_saida) {
    size_t i = 0, o = 0;
    while (entrada[i] != '\0' && o + 1 < tam_saida) {
        if (entrada[i] == '+') {
            saida[o++] = ' ';
            i++;
        } else if (entrada[i] == '%' && entrada[i+1] && entrada[i+2]) {
            char hex[3] = { entrada[i+1], entrada[i+2], '\0' };
            saida[o++] = (char) strtol(hex, NULL, 16);
            i += 3;
        } else {
            saida[o++] = entrada[i++];
        }
    }
    saida[o] = '\0';
}

/* Pega o valor de um campo dentro de um corpo tipo "nome=Ana&mensagem=Oi" */
static void pegar_campo(const char *corpo, const char *campo, char *destino, size_t tam_destino) {
    destino[0] = '\0';
    char busca[64];
    snprintf(busca, sizeof(busca), "%s=", campo);
    const char *inicio = strstr(corpo, busca);
    if (!inicio) return;
    inicio += strlen(busca);
    const char *fim = strchr(inicio, '&');
    size_t tam = fim ? (size_t)(fim - inicio) : strlen(inicio);
    if (tam >= tam_destino) tam = tam_destino - 1;

    char bruto[2048];
    if (tam >= sizeof(bruto)) tam = sizeof(bruto) - 1;
    memcpy(bruto, inicio, tam);
    bruto[tam] = '\0';

    url_decode(bruto, destino, tam_destino);
}

/* Le um arquivo inteiro para dentro de um buffer alocado (o chamador deve dar free) */
static char *ler_arquivo(const char *caminho, long *tamanho_lido) {
    FILE *f = fopen(caminho, "rb");
    if (!f) {
        *tamanho_lido = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long tam = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(tam + 1);
    if (!buf) { fclose(f); *tamanho_lido = 0; return NULL; }
    size_t lido = fread(buf, 1, tam, f);
    buf[lido] = '\0';
    fclose(f);
    *tamanho_lido = (long) lido;
    return buf;
}

/* Acrescenta uma linha "Nome: mensagem" no arquivo compartilhado chat.txt */
static void gravar_mensagem(const char *nome, const char *mensagem) {
    FILE *f = fopen(CHAT_FILE_PATH, "a");
    if (!f) {
        printf("[ERRO] Nao consegui abrir '%s' para escrita. Motivo: %s\n", CHAT_FILE_PATH, strerror(errno));
        printf("       (Dica: mova a pasta pra fora do OneDrive/Desktop, ou rode o servidor como administrador)\n");
        return;
    }
    if (strlen(nome) == 0) nome = "Anonimo";
    fprintf(f, "%s: %s\n", nome, mensagem);
    fclose(f);
    printf("[OK] Mensagem gravada em '%s': %s: %s\n", CHAT_FILE_PATH, nome, mensagem);
}

static void enviar_resposta(socket_t cliente, const char *status, const char *tipo_conteudo, const char *corpo, long tam_corpo) {
    char cabecalho[512];
    int n = snprintf(cabecalho, sizeof(cabecalho),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %ld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n", status, tipo_conteudo, tam_corpo);
    send(cliente, cabecalho, n, 0);
    if (tam_corpo > 0) send(cliente, corpo, tam_corpo, 0);
}

static void servir_arquivo_estatico(socket_t cliente, const char *caminho_disco, const char *tipo_conteudo) {
    long tam = 0;
    char *conteudo = ler_arquivo(caminho_disco, &tam);
    if (!conteudo) {
        const char *msg = "Arquivo nao encontrado no servidor.";
        enviar_resposta(cliente, "404 Not Found", "text/plain", msg, (long) strlen(msg));
        return;
    }
    enviar_resposta(cliente, "200 OK", tipo_conteudo, conteudo, tam);
    free(conteudo);
}

/* Descobre o Content-Type olhando a extensao do arquivo pedido */
static const char *tipo_por_extensao(const char *caminho) {
    const char *ponto = strrchr(caminho, '.');
    if (!ponto) return "application/octet-stream";

    if (strcmp(ponto, ".html") == 0) return "text/html";
    if (strcmp(ponto, ".css") == 0)  return "text/css";
    if (strcmp(ponto, ".js") == 0)   return "application/javascript";
    if (strcmp(ponto, ".txt") == 0)  return "text/plain";
    if (strcmp(ponto, ".png") == 0)  return "image/png";
    if (strcmp(ponto, ".jpg") == 0 || strcmp(ponto, ".jpeg") == 0 || strcmp(ponto, ".jfif") == 0) return "image/jpeg";
    if (strcmp(ponto, ".gif") == 0)  return "image/gif";
    if (strcmp(ponto, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(ponto, ".ico") == 0)  return "image/x-icon";

    return "application/octet-stream";
}

/* ---------- Tratamento de cada conexao ---------- */
static void tratar_cliente(socket_t cliente) {
    char requisicao[TAM_BUFFER];
    int recebido = recv(cliente, requisicao, sizeof(requisicao) - 1, 0);
    if (recebido <= 0) return;
    requisicao[recebido] = '\0';

    char metodo[8] = {0}, caminho[256] = {0};
    sscanf(requisicao, "%7s %255s", metodo, caminho);

    if (strcmp(metodo, "GET") == 0 && strcmp(caminho, "/api/mensagens") == 0) {
        long tam = 0;
        char *conteudo = ler_arquivo(CHAT_FILE_PATH, &tam);
        if (!conteudo) {
            enviar_resposta(cliente, "200 OK", "text/plain", "", 0);
        } else {
            enviar_resposta(cliente, "200 OK", "text/plain", conteudo, tam);
            free(conteudo);
        }

    } else if (strcmp(metodo, "GET") == 0) {
        /* Qualquer outro GET: serve o arquivo pedido direto da pasta.
           Ex: /index.html, /bonito.css, /pao-de-queijo.jfif, /chat.html ... */
        const char *nome_arquivo = (caminho[0] == '/') ? caminho + 1 : caminho;
        if (strlen(nome_arquivo) == 0) nome_arquivo = "index.html"; /* pagina principal */

        servir_arquivo_estatico(cliente, nome_arquivo, tipo_por_extensao(nome_arquivo));

    } else if (strcmp(metodo, "POST") == 0 && strcmp(caminho, "/api/enviar") == 0) {
        /* O corpo do POST vem depois da linha em branco "\r\n\r\n" */
        char *corpo = strstr(requisicao, "\r\n\r\n");
        corpo = corpo ? corpo + 4 : "";

        char nome[128], mensagem[1024];
        pegar_campo(corpo, "nome", nome, sizeof(nome));
        pegar_campo(corpo, "mensagem", mensagem, sizeof(mensagem));

        printf("[RECEBIDO] nome='%s' mensagem='%s'\n", nome, mensagem);

        if (strlen(mensagem) > 0) {
            gravar_mensagem(nome, mensagem);
        } else {
            printf("[AVISO] O campo mensagem chegou vazio, nada foi gravado.\n");
        }
        enviar_resposta(cliente, "200 OK", "text/plain", "ok", 2);

    } else {
        const char *msg = "Rota nao encontrada.";
        enviar_resposta(cliente, "404 Not Found", "text/plain", msg, (long) strlen(msg));
    }
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    socket_t servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor < 0) {
        printf("Erro ao criar o socket.\n");
        return 1;
    }

    int opcao = 1;
    setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR, (const char *) &opcao, sizeof(opcao));

    struct sockaddr_in endereco;
    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = INADDR_ANY;
    endereco.sin_port = htons(PORTA);

    if (bind(servidor, (struct sockaddr *) &endereco, sizeof(endereco)) < 0) {
        printf("Erro ao dar bind na porta %d. Ela ja esta em uso?\n", PORTA);
        return 1;
    }

    listen(servidor, 10);
    printf("Servidor do chat rodando em http://localhost:%d/chat.html\n", PORTA);
    printf("Arquivo compartilhado sendo usado: %s\n", CHAT_FILE_PATH);

    while (1) {
        struct sockaddr_in endereco_cliente;
        socklen_t tam_endereco = sizeof(endereco_cliente);
        socket_t cliente = accept(servidor, (struct sockaddr *) &endereco_cliente, &tam_endereco);
        if (cliente < 0) continue;

        tratar_cliente(cliente);
        FECHAR_SOCKET(cliente);
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}