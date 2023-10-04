#include <asf.h>
#include "conf_board.h"

#include "gfx_mono_ug_2832hsweg04.h"
#include "gfx_mono_text.h"
#include "sysfont.h"

// AV2
#include "coffee/coffee.h"


#define TEMP_SENSOR_AFEC AFEC0
#define TEMP_SENSOR_AFEC_ID ID_AFEC0
#define TEMP_SENSOR_AFEC_CHANNEL 0 // Canal do pino PD30

#define N 10 // Número de pontos na média móvel
#define READY_TO_BREW 80 // Temperatura em graus Celsius

#define BUT1_PIO PIOD
#define BUT1_PIO_ID ID_PIOD
#define BUT1_PIO_PIN 28
#define BUT1_PIO_PIN_MASK (1 << BUT1_PIO_PIN)

#define BUT2_PIO PIOC
#define BUT2_PIO_ID ID_PIOC
#define BUT2_PIO_PIN 31
#define BUT2_PIO_PIN_MASK (1 << BUT2_PIO_PIN)

#define LED1_PIO           PIOA
#define LED1_PIO_ID        ID_PIOA
#define LED1_PIO_IDX       0 // 7 no EXT
#define LED1_PIO_IDX_MASK  (1u << LED1_PIO_IDX)

#define LED2_PIO           PIOC
#define LED2_PIO_ID        ID_PIOC
#define LED2_PIO_IDX       30 // 8 no EXT
#define LED2_PIO_IDX_MASK  (1u << LED2_PIO_IDX)

#define RTT_PRESCALER 1
#define SHORT_COFFE_TIME 5
#define LONG_COFFE_TIME 10

/** RTOS  */
#define TASK_OLED_STACK_SIZE                (1024*6/sizeof(portSTACK_TYPE))
#define TASK_OLED_STACK_PRIORITY            (tskIDLE_PRIORITY)

#define TASK_TEMP_SENSOR_STACK_SIZE         (1024*6/sizeof(portSTACK_TYPE))
#define TASK_TEMP_SENSOR_STACK_PRIORITY     (tskIDLE_PRIORITY)

#define TASK_HEAT_COFFE_MACHINE_STACK_SIZE  (1024*6/sizeof(portSTACK_TYPE))
#define TASK_HEAT_COFFE_MACHINE_STACK_PRIORITY  (tskIDLE_PRIORITY)

#define TASK_BREW_COFFE_STACK_SIZE          (1024*6/sizeof(portSTACK_TYPE))
#define TASK_BREW_COFFE_STACK_PRIORITY      (tskIDLE_PRIORITY)

QueueHandle_t xQueueRawTemp;
QueueHandle_t xQueueTemp;

TimerHandle_t xTimerTempSensor;

SemaphoreHandle_t xSemaphoreBut1;
SemaphoreHandle_t xSemaphoreBut2;
SemaphoreHandle_t xReadyToBrew;
SemaphoreHandle_t xSemaphoreRTT;

/************************************************************************/
/* glboals                                                               */
/************************************************************************/


/************************************************************************/
/* PROTOtypes                                                           */
/************************************************************************/
extern void vApplicationStackOverflowHook(xTaskHandle *pxTask,  signed char *pcTaskName);
extern void vApplicationIdleHook(void);
extern void vApplicationTickHook(void);
extern void vApplicationMallocFailedHook(void);
extern void xPortSysTickHandler(void);

void vTimerTempSensorCallback(TimerHandle_t xTimerTempSensor);
static void config_AFEC(Afec *afec, uint32_t afec_id, uint32_t afec_channel, afec_callback_t callback);
static void configure_console(void);
static void BUT_init(void);
static void RTT_init(float freqPrescale, uint32_t IrqNPulses, uint32_t rttIRQSource);
void RTT_Handler(void);

/************************************************************************/
/* RTOS application funcs                                               */
/************************************************************************/

extern void vApplicationStackOverflowHook( xTaskHandle *pxTask, signed char *pcTaskName) {
	printf("stack overflow %x %s\r\n", pxTask, (portCHAR *)pcTaskName);
	for (;;) {	}
}

extern void vApplicationIdleHook(void) { }

extern void vApplicationTickHook(void) { }

extern void vApplicationMallocFailedHook(void) { configASSERT( ( volatile void * ) NULL ); }

/************************************************************************/
/* handlers / callbacks                                                 */
/************************************************************************/

static void temp_sensor_callback(void) {
	uint32_t temp;
  	temp = afec_channel_get_value(TEMP_SENSOR_AFEC, TEMP_SENSOR_AFEC_CHANNEL);
  
  	BaseType_t xHigherPriorityTaskWoken = pdTRUE;
  	xQueueSendFromISR(xQueueRawTemp, &temp, &xHigherPriorityTaskWoken);
}

void but1_callback(void) {
	BaseType_t xHigherPriorityTaskWoken = pdTRUE;
	xSemaphoreGiveFromISR(xSemaphoreBut1, &xHigherPriorityTaskWoken);
}

void but2_callback(void) {
	BaseType_t xHigherPriorityTaskWoken = pdTRUE;
	xSemaphoreGiveFromISR(xSemaphoreBut2, &xHigherPriorityTaskWoken);
}

void vTimerTempSensorCallback(TimerHandle_t xTimerTempSensor) {
	/* Selecina canal e inicializa conversão */
	afec_channel_enable(TEMP_SENSOR_AFEC, TEMP_SENSOR_AFEC_CHANNEL);
	afec_start_software_conversion(TEMP_SENSOR_AFEC);
}

/************************************************************************/
/* TASKS                                                                */
/************************************************************************/

static void task_oled(void *pvParameters) {
	gfx_mono_ssd1306_init();
	gfx_mono_draw_string("Exemplo RTOS", 0, 0, &sysfont);
	gfx_mono_draw_string("oii", 0, 20, &sysfont);	
	
	for (;;)  {
	}
}

static void task_heat_coffe_machine(void *pvParameters) {
	// configura ADC e TC para controlar a leitura
  	config_AFEC(TEMP_SENSOR_AFEC, TEMP_SENSOR_AFEC_ID, TEMP_SENSOR_AFEC_CHANNEL, temp_sensor_callback);

	xTimerTempSensor = xTimerCreate("Timer1", 1000, pdTRUE, (void *)0, vTimerTempSensorCallback);

	xTimerStart(xTimerTempSensor, 0);

	// variável para recever dados da fila
  	uint32_t temp_in_celsius;

	for(;;) {
		if (xQueueReceive(xQueueTemp, &temp_in_celsius, 1000)) {
      		if (temp_in_celsius <= READY_TO_BREW) {
				coffee_heat_on();
	  		} else {
				xSemaphoreGive(xReadyToBrew);
	  		}
		}
	}
}

static void task_temp_sensor(void *pvParameters) {
	config_AFEC(TEMP_SENSOR_AFEC, TEMP_SENSOR_AFEC_ID, TEMP_SENSOR_AFEC_CHANNEL, temp_sensor_callback); // set up the temperature sensor
	
	uint32_t temp;
	for (;;) {
		if (xQueueReceive(xQueueRawTemp, &temp, portMAX_DELAY) == pdTRUE) {
			uint32_t temp_in_celsius = 100 * temp / 4095;
			xQueueSend(xQueueTemp, &temp_in_celsius, 0);
		}
	}
}

static void task_brew_coffe(void *pvParameters) {
	for (;;) {

		for (;;) {
			if (xSemaphoreTake(xReadyToBrew, 1000) == pdTRUE) {
				BUT_init();
				break;
			}
		}

		for (;;) {
			if (xSemaphoreTake(xSemaphoreBut1, 1000) == pdTRUE) {
				if (xSemaphoreTake(xSemaphoreRTT, 10000) == pdTRUE) {
					coffe_pump_off();
					break;
				} else {
					coffe_pump_on();
					RTT_init(RTT_PRESCALER, SHORT_COFFE_TIME, RTT_MR_ALMIEN);
				}
			}

			if (xSemaphoreTake(xSemaphoreBut2, 1000) == pdTRUE) {
				if (xSemaphoreTake(xSemaphoreRTT, 10000) == pdTRUE) {
					coffe_pump_off();
					break;
				} else {
					coffe_pump_on();
					RTT_init(RTT_PRESCALER, LONG_COFFE_TIME, RTT_MR_ALMIEN);
				}
			}
		}
	}
}

/************************************************************************/
/* funcoes                                                              */
/************************************************************************/

static void configure_console(void) {
	const usart_serial_options_t uart_serial_options = {
		.baudrate = CONF_UART_BAUDRATE,
		.charlength = CONF_UART_CHAR_LENGTH,
		.paritytype = CONF_UART_PARITY,
		.stopbits = CONF_UART_STOP_BITS,
	};

	/* Configure console UART. */
	stdio_serial_init(CONF_UART, &uart_serial_options);

	/* Specify that stdout should not be buffered. */
	setbuf(stdout, NULL);
}

static void config_AFEC(Afec *afec, uint32_t afec_id, uint32_t afec_channel, afec_callback_t callback) {
  /*************************************
   * Ativa e configura AFEC
   *************************************/
  /* Ativa AFEC - 0 */
  afec_enable(afec);

  /* struct de configuracao do AFEC */
  struct afec_config afec_cfg;

  /* Carrega parametros padrao */
  afec_get_config_defaults(&afec_cfg);

  /* Configura AFEC */
  afec_init(afec, &afec_cfg);

  /* Configura trigger por software */
  afec_set_trigger(afec, AFEC_TRIG_SW);

  /*** Configuracao específica do canal AFEC ***/
  struct afec_ch_config afec_ch_cfg;
  afec_ch_get_config_defaults(&afec_ch_cfg);
  afec_ch_cfg.gain = AFEC_GAINVALUE_0;
  afec_ch_set_config(afec, afec_channel, &afec_ch_cfg);

  /*
  * Calibracao:
  * Because the internal ADC offset is 0x200, it should cancel it and shift
  down to 0.
  */
  afec_channel_set_analog_offset(afec, afec_channel, 0x200);

  /***  Configura sensor de temperatura ***/
  struct afec_temp_sensor_config afec_temp_sensor_cfg;

  afec_temp_sensor_get_config_defaults(&afec_temp_sensor_cfg);
  afec_temp_sensor_set_config(afec, &afec_temp_sensor_cfg);

  /* configura IRQ */
  afec_set_callback(afec, afec_channel, callback, 1);
  NVIC_SetPriority(afec_id, 4);
  NVIC_EnableIRQ(afec_id);
}


static void BUT_init(void) {
    /* liga o clock do button */
    pmc_enable_periph_clk(BUT1_PIO_ID);
	pmc_enable_periph_clk(BUT2_PIO_ID);

    /* conf botao como entrada */
    pio_configure(BUT1_PIO, PIO_INPUT, BUT1_PIO_PIN_MASK, PIO_PULLUP | PIO_DEBOUNCE);
	pio_configure(BUT2_PIO, PIO_INPUT, BUT2_PIO_PIN_MASK, PIO_PULLUP | PIO_DEBOUNCE);

    pio_set_debounce_filter(BUT1_PIO, BUT1_PIO_PIN_MASK, 60);
	pio_set_debounce_filter(BUT2_PIO, BUT2_PIO_PIN_MASK, 60);

    pio_handler_set(BUT1_PIO, BUT1_PIO_ID, BUT1_PIO_PIN_MASK, PIO_IT_FALL_EDGE, but1_callback);
	pio_handler_set(BUT2_PIO, BUT2_PIO_ID, BUT2_PIO_PIN_MASK, PIO_IT_FALL_EDGE, but2_callback);

    pio_enable_interrupt(BUT1_PIO, BUT1_PIO_PIN_MASK);
	pio_enable_interrupt(BUT2_PIO, BUT2_PIO_PIN_MASK);

    pio_get_interrupt_status(BUT1_PIO);
	pio_get_interrupt_status(BUT2_PIO);

    /* configura prioridae */
    NVIC_EnableIRQ(BUT1_PIO_ID);
	NVIC_EnableIRQ(BUT2_PIO_ID);

	NVIC_SetPriority(BUT1_PIO_ID, 4);
	NVIC_SetPriority(BUT2_PIO_ID, 4);
}

static void RTT_init(float freqPrescale, uint32_t IrqNPulses, uint32_t rttIRQSource) {

    uint16_t pllPreScale = (int)(((float)32768) / freqPrescale);

    rtt_sel_source(RTT, false);
    rtt_init(RTT, pllPreScale);

    if (rttIRQSource & RTT_MR_ALMIEN) {
        uint32_t ul_previous_time;
        ul_previous_time = rtt_read_timer_value(RTT);
        while (ul_previous_time == rtt_read_timer_value(RTT));
        rtt_write_alarm_time(RTT, IrqNPulses + ul_previous_time);
    }

    /* config NVIC */
    NVIC_DisableIRQ(RTT_IRQn);
    NVIC_ClearPendingIRQ(RTT_IRQn);
    NVIC_SetPriority(RTT_IRQn, 4);
    NVIC_EnableIRQ(RTT_IRQn);

    /* Enable RTT interrupt */
    if (rttIRQSource & (RTT_MR_RTTINCIEN | RTT_MR_ALMIEN))
        rtt_enable_interrupt(RTT, rttIRQSource);
    else
        rtt_disable_interrupt(RTT, RTT_MR_RTTINCIEN | RTT_MR_ALMIEN);
}

void RTT_Handler(void) {
  uint32_t ul_status;
  ul_status = rtt_get_status(RTT);

  /* IRQ due to Alarm */
  if ((ul_status & RTT_SR_ALMS) == RTT_SR_ALMS) {
    	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		xSemaphoreGiveFromISR(xSemaphoreRTT, &xHigherPriorityTaskWoken);
    }  
}

/************************************************************************/
/* main                                                                 */
/************************************************************************/

int main(void) {
	/* Initialize the SAM system */
	sysclk_init();
	board_init();

	/* Initialize the console uart */
	configure_console();

	/* Create task to control oled */
	if (xTaskCreate(task_oled, "oled", TASK_OLED_STACK_SIZE, NULL, TASK_OLED_STACK_PRIORITY, NULL) != pdPASS) {
	  printf("Failed to create oled task\r\n");
	}
	
	if (xTaskCreate(task_av2, "av2", TASK_OLED_STACK_SIZE, NULL, TASK_OLED_STACK_PRIORITY, NULL) != pdPASS) {
		printf("Failed to create oled task\r\n");
	}

	/* Create task to control temp sensor */
	if (xTaskCreate(task_temp_sensor, "temp_sensor", TASK_TEMP_SENSOR_STACK_SIZE, NULL, TASK_TEMP_SENSOR_STACK_PRIORITY, NULL) != pdPASS) {
		printf("Failed to create temp_sensor task\r\n");
	}

	/* Create task to control heat coffe machine */
	if (xTaskCreate(task_heat_coffe_machine, "heat_coffe_machine", TASK_HEAT_COFFE_MACHINE_STACK_SIZE, NULL, TASK_HEAT_COFFE_MACHINE_STACK_PRIORITY, NULL) != pdPASS) {
		printf("Failed to create heat_coffe_machine task\r\n");
	}

	/* Create task to control brew coffe */
	if (xTaskCreate(task_brew_coffe, "brew_coffe", TASK_BREW_COFFE_STACK_SIZE, NULL, TASK_BREW_COFFE_STACK_PRIORITY, NULL) != pdPASS) {
		printf("Failed to create brew_coffe task\r\n");
	}

	/* Create queue to temp sensor */
	xQueueRawTemp = xQueueCreate(10, sizeof(uint32_t));
	if (xQueueRawTemp == NULL) {
		printf("Failed to create temp_sensor queue\r\n");
	}

	/* Create queue to temp sensor */
	xQueueTemp = xQueueCreate(10, sizeof(uint32_t));
	if (xQueueTemp == NULL) {
		printf("Failed to create temp_sensor queue\r\n");
	}

	/* Create semaphore to ready to brew */
	xReadyToBrew = xSemaphoreCreateBinary();
	if (xReadyToBrew == NULL) {
		printf("Failed to create ready to brew semaphore\r\n");
	}

	/* Create semaphore to but1 */
	xSemaphoreBut1 = xSemaphoreCreateBinary();
	if (xSemaphoreBut1 == NULL) {
		printf("Failed to create but1 semaphore\r\n");
	}

	/* Create semaphore to but2 */
	xSemaphoreBut2 = xSemaphoreCreateBinary();
	if (xSemaphoreBut2 == NULL) {
		printf("Failed to create but2 semaphore\r\n");
	}

	xSemaphoreRTT = xSemaphoreCreateBinary();
	if (xSemaphoreRTT == NULL) {
		printf("Failed to create RTT semaphore\r\n");
	}

	/* Start the scheduler. */
	vTaskStartScheduler();

	/* RTOS n�o deve chegar aqui !! */
	while(1){}

	/* Will only get here if there was insufficient memory to create the idle task. */
	return 0;
}
