#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "ssd1306/ssd1306.h"
#include "ssd1306/ssd1306_fonts.h"
#include "joystick.c"

// = = = = = = = = = = = = = =

#define INTERVALO_CALLBACK 100

// ===== PINOS =====
#define LED_GREEN 11
#define LED_RED 13 // Define o pino do LED
#define LED_BLUE 12

#define BT_A 5 // Pino do botão 1
#define BT_B 6 // Pino do botão 2

#define TRIG_PIN 9            // pino conectado ao fio de trigger do sensor ultrassônico
#define ECHO_PIN 8            // pino conectado ao fio de echo do sensor ultrassônico
#define ECHO_TIMEOUT_US 30000 // tempo máximo para esperar o echo (em microssegundos)
#define BUFFER_SIZE 5         // número de leituras para calcular a média do peso

// ===== WIFI =====
#define WIFI_SSID "LabSS"
#define WIFI_PASSWORD "$3nh4@L@8"

//--Wifi LATICÍNIO
//LabSS
//$3nh4@L@8

//--Wifi LAICA
//tarefa-mqtt
//laica@2025

// ===== MQTT =====
#define MQTT_SERVER "10.1.1.33"
#define MQTT_BROKER_PORT 1883
#define TOPIC_RX "sertao_serido/fila"
#define TOPIC_TX "sertao_serido/leite"

//mqtt_server_ip laticínio: 10.1.1.152

// ===== MQTT RX =====
#define RX_BUFFER_SIZE 128
char mqtt_rx_buffer[RX_BUFFER_SIZE];
uint16_t mqtt_rx_len = 0;
char nome_atual[20] = "Aguardando";

// ===== ESTADO =====
mqtt_client_t *client;
ip_addr_t mqtt_server_ip;
bool mqtt_connected = false;
bool mqtt_connecting = false;
absolute_time_t last_publish;
absolute_time_t last_mqtt_reconnect_attempt;
absolute_time_t last_status_log;
absolute_time_t mqtt_ignore_rx_until;
bool mqtt_ignore_initial_rx = false;
bool aguardando_peso = false;
bool peso_ja_enviado = false;

// Variáveis globais
bool BT_A_state = true; // estado do botão A
bool BT_B_state = true; // estado do botão B
volatile bool cancelar_coleta = false; // flag de cancelamento de coleta pelo botão B

int menu = 0;
bool conectar = true;

float calibrador = 60.0f; // altura do sensor em relacao ao fundo (cm)

bool envia1 = false;
bool envia2 = false;

bool confirmar = false;

int tempo = 0;

float buffer_distancia[BUFFER_SIZE] = {0};
int indice_buffer = 0;
int total_amostras_buffer = 0;

void mqtt_connection_cb(mqtt_client_t *client,
                        void *arg,
                        mqtt_connection_status_t status);

void sinalizar(char *status);

static struct mqtt_connect_client_info_t mqtt_ci = {
    .client_id = "pico_balanca",
    .keep_alive = 30};

void iniciar_conexao_mqtt(void)
{
    if (mqtt_connected || mqtt_connecting)
    {
        return;
    }

    if (!ipaddr_aton(MQTT_SERVER, &mqtt_server_ip))
    {
        printf("MQTT: IP invalido no MQTT_SERVER: %s\n", MQTT_SERVER);
        return;
    }

    err_t err = mqtt_client_connect(client,
                                    &mqtt_server_ip,
                                    MQTT_BROKER_PORT,
                                    mqtt_connection_cb,
                                    NULL,
                                    &mqtt_ci);

    if (err != ERR_OK)
    {
        printf("MQTT: falha ao iniciar conexao (err=%d) broker=%s:%d\n",
               err,
               MQTT_SERVER,
               MQTT_BROKER_PORT);
    }
    else
    {
        mqtt_connecting = true;
        printf("MQTT: tentando conectar em %s:%d ...\n", MQTT_SERVER, MQTT_BROKER_PORT);
    }
}

// Função para monitorar o estado dos botões
bool monitor_buttons(struct repeating_timer *t)
{
    static bool button1_last_state = false;
    static bool button2_last_state = false;

    bool button1_state = !gpio_get(BT_A); // Botão pressionado = LOW
    bool button2_state = !gpio_get(BT_B);

    if (menu == 0)
    {
        if (button1_state != button1_last_state)
        {
            button1_last_state = button1_state;
            if (button1_state)
            {
                // printf("Botão 1 pressionado\n");
            }
            else
            {
                // printf("Botão 1 solto\n");
                BT_A_state = !BT_A_state;
                if (confirmar)
                {
                    envia1 = true;
                }
                if (BT_A_state)
                {
                }
            }
        }

        if (button2_state != button2_last_state)
        {
            button2_last_state = button2_state;
            if (button2_state)
            {
                // printf("Botão 2 pressionado\n");
            }
            else
            {
                // printf("Botão 2 solto\n");
                if (aguardando_peso)
                {
                    cancelar_coleta = true;
                }
                else
                {
                    BT_B_state = !BT_B_state;
                    if (BT_B_state)
                    {
                        envia2 = false;
                    }
                }
            }
        }
    }

    joystick_read_axis(&vry_value, &vrx_value);

    return true;
}

// Função chamada na interrupção
void flow_sensor_isr(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;
}

void esquematico_menu()
{
    char buffer[20];

    if (menu == 1)
    {
        ssd1306_FillRectangle(0, 0, 127, 63, Black);
        ssd1306_DrawRectangle(0, 0, 127, 63, 1);
        ssd1306_SetCursor(22, 10);
        ssd1306_WriteString("ALT. SENSOR", Font_7x10, 1);
        ssd1306_DrawRectangle(25, 27, 103, 27, 1);

        sprintf(buffer, "%.1f\n", calibrador);
        ssd1306_FillRectangle(2, 39, 125, 53, Black);
        ssd1306_SetCursor((int)(64 - ((strlen(buffer) * 7) / 2)), 40);
        ssd1306_WriteString(buffer, Font_7x10, 1);
    }
    else if (menu == 2)
    {
        ssd1306_FillRectangle(0, 0, 127, 63, Black);
        ssd1306_DrawRectangle(0, 0, 127, 63, 1);
        ssd1306_SetCursor(18, 10);
        ssd1306_WriteString("  ID COLETA  ", Font_7x10, 1);
        ssd1306_DrawRectangle(14, 27, 114, 27, 1);

        sprintf(buffer, "%s\n", nome_atual);
        ssd1306_FillRectangle(2, 39, 125, 53, Black);
        ssd1306_SetCursor((int)(64 - ((strlen(buffer) * 7) / 2)), 40);
        ssd1306_WriteString(buffer, Font_7x10, 1);
    }

    ssd1306_UpdateScreen();
}

// Função para exibir o esqueleto de separação no display
void esquematico_Display(bool active2)
{

    char buffer[20];

    int min = tempo / 60;
    int seg = tempo % 60;

    ssd1306_DrawRectangle(0, 0, 127, 63, 1);

    if (active2)
    {
        ssd1306_DrawRectangle(2, 2, 125, 61, 1);
        ssd1306_SetCursor(39, 10);
        ssd1306_WriteString("ATIVO", Font_7x10, 1);
        ssd1306_SetCursor(18, 24);
        ssd1306_WriteString("Aguardando", Font_7x10, 1);
    }
    else
    {
        ssd1306_FillRectangle(2, 2, 125, 61, Black);
        ssd1306_SetCursor(20, 24);
        ssd1306_WriteString("DESATIVADO", Font_7x10, 1);
    }

    ssd1306_UpdateScreen();
}

void exibir_no_oled(float distancia, float volume_cm3)
{
    char linha1[24];
    char linha2[24];
    char linha3[24];
    float volume_l = volume_cm3 / 1000.0f;

    ssd1306_FillRectangle(3, 3, 124, 60, Black);

    if (BT_B_state)
    {
        snprintf(linha1, sizeof(linha1), "ID: %s", nome_atual);
        snprintf(linha2, sizeof(linha2), "Dist: %.2f cm", distancia);
        snprintf(linha3, sizeof(linha3), "Vol: %.2f L", volume_l);

        ssd1306_SetCursor(8, 8);
        ssd1306_WriteString(linha1, Font_6x8, 1);
        ssd1306_SetCursor(8, 20);
        ssd1306_WriteString(linha2, Font_6x8, 1);
        ssd1306_SetCursor(8, 30);
        ssd1306_WriteString(linha3, Font_6x8, 1);

        if (confirmar)
        {
            ssd1306_FillRectangle(5, 45, 122, 55, Black);
            ssd1306_SetCursor(39, 45);
            ssd1306_WriteString("ENVIAR?", Font_7x10, 1);
        }
    }
    else
    {
        ssd1306_SetCursor(18, 20);
        ssd1306_WriteString("ENVIO", Font_7x10, 1);
        ssd1306_SetCursor(8, 34);
        ssd1306_WriteString("DESATIVADO", Font_7x10, 1);
    }

    ssd1306_UpdateScreen(); // Atualiza a tela OLED
}

void sinalizar(char *status)
{

    if (status == "desconectado")
    {
        gpio_put(LED_RED, 1);
        gpio_put(LED_GREEN, 0);
        gpio_put(LED_BLUE, 0);
    }
    else if (status == "conectado")
    {
        gpio_put(LED_RED, 0);
        gpio_put(LED_GREEN, 0);
        gpio_put(LED_BLUE, 1);
    }
    else if (status == "apto")
    {
        gpio_put(LED_RED, 0);
        gpio_put(LED_GREEN, 1);
        gpio_put(LED_BLUE, 1);
    }
    else if (status == "medindo")
    {
        gpio_put(LED_RED, 1);
        gpio_put(LED_GREEN, 1);
        gpio_put(LED_BLUE, 0);
    }
    else if (status == "enviando")
    {
        gpio_put(LED_RED, 0);
        gpio_put(LED_GREEN, 1);
        gpio_put(LED_BLUE, 0);
    }
    else if (status == "off")
    {
        gpio_put(LED_RED, 0);
        gpio_put(LED_GREEN, 0);
        gpio_put(LED_BLUE, 0);
    }
    else if (status == "mqtt")
    {
        gpio_put(LED_RED, 1);
        gpio_put(LED_GREEN, 0);
        gpio_put(LED_BLUE, 1);
    }
}

void setup_sensor(uint sensor_pin)
{
    // Configura o pino do sensor como entrada com pull-up
    gpio_init(sensor_pin);
    gpio_set_dir(sensor_pin, GPIO_IN);
    gpio_pull_up(sensor_pin);

    // Configura interrupção para borda de descida (pulso do sensor)
    gpio_set_irq_enabled_with_callback(sensor_pin, GPIO_IRQ_EDGE_FALL, true, &flow_sensor_isr);
}

void medir_volume(uint sensor_pin, uint32_t pulse_count, bool *last_reg, float *contador_volume, bool *enviar)
{
    // Cada pulso = 1/450 L (ajustar conforme especificação)
    float frequency = pulse_count;                    // pulsos por segundo
    float flow_rate = frequency / calibrador * 60.0f; // Litros/minuto

    if (!(*enviar))
    {
        printf("Sensor: %u | Pulsos: %lu | Vazao: %.2f L/min | Volume: %.2fL | Tempo: %ds | %d\n ", sensor_pin, pulse_count, flow_rate, *contador_volume, tempo, envia1);

        // Inicia a medição do volume
        if (!(*last_reg) && pulse_count != 0)
        {
            *last_reg = true;
            *contador_volume += flow_rate / 60;
            tempo += 1;
            sinalizar("medindo");
        }

        // Finaliza a medição do volume quando o fluxo é interrompido
        // e envia o valor medido para o thingspeak
        if (*last_reg)
        {
            if (pulse_count == 0)
            {
                *last_reg = false;
                *enviar = true;
                sinalizar("enviando");
            }
            else
            {
                *contador_volume += flow_rate / 60;
                tempo += 1;
            }
        }
    }
}

// ===== MQTT DATA CALLBACK =====
void mqtt_incoming_data_cb(void *arg,
                           const u8_t *data,
                           u16_t len,
                           u8_t flags)
{
    if (mqtt_rx_len + len < RX_BUFFER_SIZE)
    {
        memcpy(mqtt_rx_buffer + mqtt_rx_len, data, len);
        mqtt_rx_len += len;
    }

    if (flags & MQTT_DATA_FLAG_LAST)
    {
        if (mqtt_ignore_initial_rx && time_reached(mqtt_ignore_rx_until))
        {
            mqtt_ignore_initial_rx = false;
        }

        if (mqtt_ignore_initial_rx)
        {
            printf("MQTT RX ignorado no inicio (possivel mensagem retida)\n");
            mqtt_rx_len = 0;
            return;
        }

        mqtt_rx_buffer[mqtt_rx_len] = '\0';
        printf("MQTT RX: %s\n", mqtt_rx_buffer);

        // Extrair "id"
        char *p = strstr(mqtt_rx_buffer, "\"id\"");
        if (p)
        {
            p = strchr(p, ':');
            if (p)
            {
                p++;
                while (*p == ' ' || *p == '\"')
                    p++;

                char *end = strchr(p, '}');
                if (end)
                {
                    size_t n = end - p;
                    if (n < sizeof(nome_atual))
                    {
                        strncpy(nome_atual, p, n);
                        nome_atual[n] = '\0';

                        aguardando_peso = true;
                        peso_ja_enviado = false;

                        printf("Produtor: %s. Aguardando peso...\n\n", nome_atual);
                        sinalizar("off");
                        sleep_ms(150);
                        sinalizar("apto");
                        sleep_ms(100);
                        sinalizar("off");
                    }
                }
            }
        }

        mqtt_rx_len = 0;
    }
}

// ===== MQTT TOPIC CALLBACK =====
void mqtt_incoming_publish_cb(void *arg,
                              const char *topic,
                              u32_t tot_len)
{
    if (strcmp(topic, TOPIC_RX) == 0)
    {
        mqtt_rx_len = 0;
    }
}

// ===== MQTT CONNECTION =====
void mqtt_connection_cb(mqtt_client_t *client,
                        void *arg,
                        mqtt_connection_status_t status)
{
    mqtt_connecting = false;
    printf("MQTT status: %d\n", status);

    if (status == MQTT_CONNECT_ACCEPTED)
    {
        mqtt_connected = true;
        aguardando_peso = false;
        peso_ja_enviado = false;
        confirmar = false;
        strcpy(nome_atual, "Aguardando");
        mqtt_ignore_initial_rx = true;
        mqtt_ignore_rx_until = delayed_by_ms(get_absolute_time(), 2000);

        printf("MQTT Conectado!\n");

        sinalizar("off");
        sleep_ms(150);
        sinalizar("mqtt");
        sleep_ms(100);
        sinalizar("off");

        mqtt_set_inpub_callback(
            client,
            mqtt_incoming_publish_cb,
            mqtt_incoming_data_cb,
            NULL);

        mqtt_subscribe(client, TOPIC_RX, 0, NULL, NULL);
    }
    else
    {
        mqtt_connected = false;
        mqtt_ignore_initial_rx = false;
        printf("Erro MQTT: conexao falhou com status=%d\n", status);
    }
}

// ===== PUBLICAÇÃO =====
void publish_peso(float vol2)
{
    float peso = vol2;

    char msg[128];

    int len = snprintf(msg, sizeof(msg),
                       "{\"id\":%s,\"peso\":%.1f}",
                       nome_atual, peso);

    mqtt_publish(client, TOPIC_TX, msg, len, 0, 0, NULL, NULL);

    char l1[32], l2[32];
    snprintf(l1, sizeof(l1), "Nome: %s", nome_atual);
    snprintf(l2, sizeof(l2), "Peso: %.1f kg", peso);

    printf("Enviado MQTT\n %s\n %s\n\n", l1, l2);

    sinalizar("off");
    sleep_ms(150);
    sinalizar("enviando");
    sleep_ms(100);
    sinalizar("off");
}

//============= CÓDIGO DO SENSOR DE DISTÂNCIA =============
// Função para medir distância
float medir_distancia_cm()
{
    gpio_put(TRIG_PIN, 0);
    sleep_us(2);

    gpio_put(TRIG_PIN, 1);
    sleep_us(10);
    gpio_put(TRIG_PIN, 0);

    // Espera ECHO subir
    absolute_time_t timeout = make_timeout_time_us(ECHO_TIMEOUT_US);
    while (gpio_get(ECHO_PIN) == 0)
    {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return -1.0f;
        }
    }

    absolute_time_t start = get_absolute_time();

    // Espera ECHO descer
    timeout = make_timeout_time_us(ECHO_TIMEOUT_US);
    while (gpio_get(ECHO_PIN) == 1)
    {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return -1.0f;
        }
    }

    absolute_time_t end = get_absolute_time();

    int64_t tempo = absolute_time_diff_us(start, end);

    // Distância em cm
    float distancia = (tempo * 0.0343) / 2;

    return distancia;
}

// Função para calcular média móvel
float calcular_media_movel(float nova_leitura)
{
    buffer_distancia[indice_buffer] = nova_leitura;
    indice_buffer = (indice_buffer + 1) % BUFFER_SIZE;

    if (total_amostras_buffer < BUFFER_SIZE)
    {
        total_amostras_buffer++;
    }

    float soma = 0.0f;
    for (int i = 0; i < total_amostras_buffer; i++)
    {
        soma += buffer_distancia[i];
    }

    return soma / total_amostras_buffer;
}

float volumeLeite(float distancia)
{
    // Largura do recipiente (em cm)
    const float largura = 79.0;
    // Comprimento do recipiente (em cm)
    const float comprimento = 120.0;
    // Altura do sensor em relação ao fundo (em cm)
    const float altura_sensor = calibrador;
    // Margem de erro máxima (em cm)
    const float margem_minima = 20.0;
    // Altura máxima útil do leite (em cm)
    const float altura_maxima_util = altura_sensor - margem_minima;

    // Calcula a altura do leite com base na distância medida
    float altura_leite = altura_sensor - distancia;

    // Limita entre 0 e altura máxima útil
    if (altura_leite < 0.0f)
    {
        altura_leite = 0.0f;
    }
    if (altura_leite > altura_maxima_util)
    {
        altura_leite = altura_maxima_util;
    }

    // Calcula o volume do leite (em cm³)
    float volume = largura * comprimento * altura_leite;
    return volume;
}
// ===== MAIN =====
int main()
{
    stdio_init_all();
    sleep_ms(2000);

    gpio_init(LED_GREEN);
    gpio_set_dir(LED_GREEN, GPIO_OUT);

    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);

    gpio_init(LED_BLUE);
    gpio_set_dir(LED_BLUE, GPIO_OUT);

    gpio_init(BT_A);
    gpio_set_dir(BT_A, GPIO_IN);
    gpio_pull_up(BT_A);

    gpio_init(BT_B);
    gpio_set_dir(BT_B, GPIO_IN);
    gpio_pull_up(BT_B);

    gpio_init(TRIG_PIN);
    gpio_set_dir(TRIG_PIN, GPIO_OUT);
    gpio_put(TRIG_PIN, 0);

    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);

    setup_joystick();
    ssd1306_Init();
    esquematico_Display(1);

    struct repeating_timer timer;
    add_repeating_timer_ms(INTERVALO_CALLBACK, monitor_buttons, NULL, &timer); // timer que verifica os botões pressionados

    printf("Sistema iniciando...");
    sinalizar("desconectado");

    // WIFI
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();

    if (cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID,
            WIFI_PASSWORD,
            CYW43_AUTH_WPA2_AES_PSK,
            30000))
    {
        printf("Erro Wi-Fi \n");
        while (1)
            ;
    }

    printf("Wi-Fi conectado! %s", WIFI_SSID);
    sinalizar("conectado");

    // MQTT
    client = mqtt_client_new();
    iniciar_conexao_mqtt();

    last_publish = get_absolute_time();
    last_mqtt_reconnect_attempt = get_absolute_time();
    last_status_log = get_absolute_time();

    float volume_atual_cm3 = 0;
    float distancia_suavizada = 0;

    while (true)
    {

        cyw43_arch_poll(); // mantém Wi-Fi/MQTT vivos

        if (!mqtt_connected && absolute_time_diff_us(last_mqtt_reconnect_attempt, get_absolute_time()) > 5000000)
        {
            last_mqtt_reconnect_attempt = get_absolute_time();
            iniciar_conexao_mqtt();
        }

        if (absolute_time_diff_us(last_status_log, get_absolute_time()) > 2000000)
        {
            last_status_log = get_absolute_time();
            printf("ESTADO mqtt=%d conectando=%d aguardando=%d pendente_envio=%d\n",
                   mqtt_connected,
                   mqtt_connecting,
                   aguardando_peso,
                   !peso_ja_enviado);
        }

        if (menu == 0)
        {
            if (cancelar_coleta)
            {
                cancelar_coleta = false;
                aguardando_peso = false;
                peso_ja_enviado = false;
                confirmar = false;
                envia1 = false;
                tempo = 0;
                strcpy(nome_atual, "Aguardando");

                ssd1306_FillRectangle(0, 0, 127, 63, Black);
                ssd1306_DrawRectangle(0, 0, 127, 63, 1);
                ssd1306_SetCursor(28, 28);
                ssd1306_WriteString("CANCELADO", Font_7x10, 1);
                ssd1306_UpdateScreen();

                sinalizar("off");
                sleep_ms(150);
                sinalizar("desconectado"); // LED vermelho ativo
                sleep_ms(150);
                sinalizar("off");
                sleep_ms(1700);

                sinalizar("conectado");

                ssd1306_FillRectangle(0, 0, 127, 63, Black);
                ssd1306_UpdateScreen();
                esquematico_Display(BT_B_state);
                continue;
            }

            if (mqtt_connected && aguardando_peso && !peso_ja_enviado)
            {
                if (BT_B_state)
                {
                    float distancia = medir_distancia_cm();

                    if (distancia < 0.0f)
                    {
                        volume_atual_cm3 = 0;
                        confirmar = false;
                        ssd1306_FillRectangle(3, 3, 124, 60, Black);
                        ssd1306_SetCursor(18, 24);
                        ssd1306_WriteString("Sensor sem", Font_7x10, White);
                        ssd1306_SetCursor(24, 36);
                        ssd1306_WriteString("resposta", Font_7x10, White);
                        ssd1306_UpdateScreen();
                        sleep_ms(300);
                        continue;
                    }

                    distancia_suavizada = calcular_media_movel(distancia);
                    volume_atual_cm3 = volumeLeite(distancia_suavizada);

                    confirmar = (volume_atual_cm3 > 1.0f);
                    tempo += 1;
                    sinalizar("medindo");

                    printf("Distancia: %.2f cm | Media: %.2f cm | Volume: %.2f L\n",
                           distancia,
                           distancia_suavizada,
                           volume_atual_cm3 / 1000.0f);

                    sleep_ms(1000);
                }
                else
                {
                    volume_atual_cm3 = 0;
                    confirmar = false;
                    sinalizar("off");
                }

                if (envia1 && BT_B_state && confirmar)
                {
                    if (conectar)
                    {
                        if (mqtt_connected && aguardando_peso && !peso_ja_enviado && absolute_time_diff_us(last_publish, get_absolute_time()) > 1000000)
                        {
                            last_publish = get_absolute_time();
                            publish_peso(volume_atual_cm3 / 1000.0f);
                            printf("\n\nPeso Publicado\n\n");

                            peso_ja_enviado = true;
                            aguardando_peso = false;
                            confirmar = false;
                            envia1 = false;
                            tempo = 0;

                            strcpy(nome_atual, "Aguardando");

                            ssd1306_FillRectangle(0, 0, 127, 63, Black);
                            ssd1306_DrawRectangle(0, 0, 127, 63, 1);
                            ssd1306_SetCursor(35, 28);
                            ssd1306_WriteString("ENVIADO", Font_7x10, 1);
                            ssd1306_UpdateScreen();

                            sleep_ms(2000);
                            sinalizar("conectado");

                            // Limpa e redesenha o estado ATIVO/Aguardando de forma limpa
                            ssd1306_FillRectangle(0, 0, 127, 63, Black);
                            ssd1306_UpdateScreen();
                            esquematico_Display(BT_B_state);
                            continue;
                        }
                    }
                }

                exibir_no_oled(distancia_suavizada, volume_atual_cm3);
            }
            else
            {
                esquematico_Display(BT_B_state);
                sleep_ms(200);
            }
        }
        else
        {
            esquematico_menu();
            if (menu == 1)
            {
                change_value(vry_value, &calibrador);
                sleep_ms(100);
            }
        }

        if (!BT_B_state)
        {
            if (vrx_value > 4000)
            {
                if (menu == 2)
                {
                    menu = 0;
                }
                else
                {
                    menu += 1;
                }
                ssd1306_FillRectangle(0, 0, 127, 63, Black);
                ssd1306_UpdateScreen();
                sleep_ms(100);
            }
            else if (vrx_value < 100)
            {
                if (menu == 0)
                {
                    menu = 2;
                }
                else
                {
                    menu -= 1;
                }
                ssd1306_FillRectangle(0, 0, 127, 63, Black);
                ssd1306_UpdateScreen();
                sleep_ms(100);
            }
        }
    }
}
