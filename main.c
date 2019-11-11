#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "acceleration.h"
#include "board_io.h"
#include "common_macros.h"
#include "event_groups.h"
#include "ff.h"
#include "gpio.h"
#include "queue.h"
#include "sj2_cli.h"
#include <stdint.h>
#include <string.h>

#define BIT_1 (1 << 0)
#define BIT_2 (1 << 1)

extern unsigned cpu_percentage;

static void blink_task(void *params);
static void uart_task(void *params);
static void sd_producer(void *p);
static void sd_consumer(void *p);
static void task_watchdog(void *p);

static gpio_s led0, led1;

QueueHandle_t sd_card;
EventGroupHandle_t watchdog;
typedef enum type { Value, Watchdog } type;
EventBits_t bits;

int main(void) {
  // led0 = board_io__get_led0();
  // led1 = board_io__get_led1();

  // xTaskCreate(blink_task, "led0", configMINIMAL_STACK_SIZE, (void *)&led0, PRIORITY_LOW, NULL);
  // xTaskCreate(blink_task, "led1", configMINIMAL_STACK_SIZE, (void *)&led1, PRIORITY_LOW, NULL);

  // It is advised to either run the uart_task, or the SJ2 command-line (CLI), but not both
  // Change '#if 0' to '#if 1' and vice versa to try it out
#if 0
  // printf() takes more stack space, size this tasks' stack higher
  xTaskCreate(uart_task, "uart", (512U * 8) / sizeof(void *), NULL, PRIORITY_LOW, NULL);
#else
  sj2_cli__init();
  // UNUSED(uart_task); // uart_task is un-used in if we are doing cli init()
#endif

  sd_card = xQueueCreate(100, sizeof(acceleration__axis_data_s));
  watchdog = xEventGroupCreate();
  acceleration__init();
  // xTaskCreate(sd_producer, "producer", 1024U, NULL, PRIORITY_MEDIUM, NULL);
  // xTaskCreate(sd_consumer, "consumer", 1024U, NULL, PRIORITY_MEDIUM, NULL);
  // xTaskCreate(task_watchdog, "watchdog", 1024U, NULL, PRIORITY_HIGH, NULL);
  gpio__construct_with_function(GPIO__PORT_0, 10, GPIO__FUNCTION_2);
  gpio__construct_with_function(GPIO__PORT_0, 11, GPIO__FUNCTION_2);
  puts("Starting RTOS");
  // vTaskStartScheduler(); // This function never returns unless RTOS scheduler runs out of memory and fails
  while (1)
    ;
  return 0;
}

static void task_watchdog(void *p) {
  while (1) {
    vTaskDelay(1000);
    bits = xEventGroupWaitBits(watchdog, BIT_1 | BIT_2, pdTRUE, pdTRUE, 1);
    if ((bits & (BIT_1 | BIT_2)) == (BIT_2 | BIT_1)) { // printf("Watchdog - Success\n");
    } else {
      // printf("Watchdog - Failure\n");
    }
  }
}

static void sd_producer(void *p) {
  while (1) {
    acceleration__axis_data_s data;
    acceleration__axis_data_s data_temp = {0, 0, 0};

    for (int i = 1; i <= 100; i++) {
      data = acceleration__get_data();
      data_temp.x += data.x;
      data_temp.y += data.y;
      data_temp.z += data.z;
    }

    data.x = data_temp.x / 100;
    data.y = data_temp.y / 100;
    data.z = data_temp.z / 100;
    xEventGroupSetBits(watchdog, BIT_1);
    xQueueSend(sd_card, &data, 1);
  }
}

static void sd_consumer(void *p) {
  acceleration__axis_data_s data;
  TickType_t initial_tick;
  while (1) {
    xQueueReceive(sd_card, &data, portMAX_DELAY);
    // printf("Value: %d %d %d Time: %d,CPU %d, WatchDog Status: Bit1 %d, Bit2 %d \n", data.x, data.y, data.z,
    //     cpu_percentage, xTaskGetTickCount(), (bits & (1 << 0)), (bits >> 1 & (1 << 0)));
    initial_tick = xTaskGetTickCount();
    while (!((xTaskGetTickCount() - initial_tick) >= 1000)) // waiting for 1 sec
      ;
    write_file_using_fatfs_pi(data);
    fflush(stdout);
    xEventGroupSetBits(watchdog, BIT_2);
  }
}

static void blink_task(void *params) {
  const gpio_s led = *((gpio_s *)params);

  // Warning: This task starts with very minimal stack, so do not use printf() API here to avoid stack overflow
  while (true) {
    gpio__toggle(led);
    vTaskDelay(500);
  }
}

void write_file_using_fatfs_pi(acceleration__axis_data_s data) {
  const char *filename = "file3.txt";
  FIL file; // File handle
  UINT bytes_written = 0;
  FRESULT result = f_open(&file, filename, (FA_WRITE | FA_OPEN_APPEND));

  if (FR_OK == result) {
    char string[264];

    sprintf(string, "Value, %d %d %d,CPU: %d, Time: %d,Status: Bit1 %d, Bit2 %d \n", data.x, data.y, data.z,
            cpu_percentage, xTaskGetTickCount(), (bits & (1 << 0)), (bits >> 1 & (1 << 0)));

    if (FR_OK == f_write(&file, string, strlen(string), &bytes_written)) {
    } else {
      printf("ERROR: Failed to write data to file\n");
    }
    f_close(&file);
  } else {
    // printf("ERROR: Failed to open: %s\n", filename);
  }
}

// This sends periodic messages over printf() which uses system_calls.c to send them to UART0
static void uart_task(void *params) {
  TickType_t previous_tick = 0;
  TickType_t ticks = 0;

  while (true) {
    // This loop will repeat at precise task delay, even if the logic below takes variable amount of ticks
    vTaskDelayUntil(&previous_tick, 2000);

    /* Calls to fprintf(stderr, ...) uses polled UART driver, so this entire output will be fully
     * sent out before this function returns. See system_calls.c for actual implementation.
     *
     * Use this style print for:
     *  - Interrupts because you cannot use printf() inside an ISR
     *  - During debugging in case system crashes before all output of printf() is sent
     */
    ticks = xTaskGetTickCount();
    fprintf(stderr, "%u: This is a polled version of printf used for debugging ... finished in", (unsigned)ticks);
    fprintf(stderr, " %lu ticks\n", (xTaskGetTickCount() - ticks));

    /* This deposits data to an outgoing queue and doesn't block the CPU
     * Data will be sent later, but this function would return earlier
     */
    ticks = xTaskGetTickCount();
    printf("This is a more efficient printf ... finished in");
    printf(" %lu ticks\n\n", (xTaskGetTickCount() - ticks));
  }
}
