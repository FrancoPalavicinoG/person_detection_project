/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "main_functions.h"

#include "detection_responder.h"
#include "image_provider.h"
#include "model_settings.h"
#include "person_detect_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include "esp_main.h"

#include <xtensa/core-macros.h>
// Globals, used for compatibility with Arduino-style sketches.
namespace {
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;

// In order to use optimized tensorflow lite kernels, a signed int8_t quantized
// model is preferred over the legacy unsigned model format. This means that
// throughout this project, input images must be converted from unisgned to
// signed format. The easiest and quickest way to convert from unsigned to
// signed 8-bit integers is to subtract 128 from the unsigned value to get a
// signed value.

#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr int scratchBufSize = 40 * 1024;
#else
constexpr int scratchBufSize = 9000;
#endif
// An area of memory to use for input, output, and intermediate arrays.
constexpr int kTensorArenaSize = 81 * 1024 + scratchBufSize;
static uint8_t *tensor_arena;//[kTensorArenaSize]; // Maybe we should move this to external
}  // namespace

void enable_instruction_counter() {
    unsigned int icount_level;
    // Leer el valor actual del ICOUNTLEVEL
    RSR(ICOUNTLEVEL, icount_level);
    // Establecer el ICOUNTLEVEL a 2 para habilitar el contador de instrucciones
    // El valor 2 es el nivel predeterminado para habilitar el contador en muchas configuraciones de Xtensa
    WSR(ICOUNTLEVEL, 2);
}
// The name of this function is important for Arduino compatibility.
void setup() {
  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  model = tflite::GetModel(g_person_detect_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Model provided is schema version %d not equal to supported "
                "version %d.", model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  if (tensor_arena == NULL) {
    tensor_arena = (uint8_t *) heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (tensor_arena == NULL) {
    printf("Couldn't allocate memory of %d bytes\n", kTensorArenaSize);
    return;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  //
  // tflite::AllOpsResolver resolver;
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroMutableOpResolver<9> micro_op_resolver;
  micro_op_resolver.AddAveragePool2D();
  micro_op_resolver.AddConv2D();
  micro_op_resolver.AddDepthwiseConv2D();
  micro_op_resolver.AddReshape();
  micro_op_resolver.AddSoftmax();
  micro_op_resolver.AddMaxPool2D(); 
  micro_op_resolver.AddFullyConnected(); 
  micro_op_resolver.AddLogistic(); 
  micro_op_resolver.AddQuantize();

  // Build an interpreter to run the model with.
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize);
  interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    MicroPrintf("AllocateTensors() failed");
    return;
  }

  // Get information about the memory area to use for the model's input.
  input = interpreter->input(0);

#ifndef CLI_ONLY_INFERENCE
  // Initialize Camera
  TfLiteStatus init_status = InitCamera();
  if (init_status != kTfLiteOk) {
    MicroPrintf("InitCamera failed\n");
    return;
  }
#endif
}

#ifndef CLI_ONLY_INFERENCE
// The name of this function is important for Arduino compatibility.
void loop() {
  // Get image from provider.
  if (kTfLiteOk != GetImage(kNumCols, kNumRows, kNumChannels, input->data.int8)) {
    MicroPrintf("Image capture failed.");
  }

  // Run the model on this input and make sure it succeeds.
  if (kTfLiteOk != interpreter->Invoke()) {
    MicroPrintf("Invoke failed.");
  }

  TfLiteTensor* output = interpreter->output(0);

  // Process the inference results.
  int8_t person_score = output->data.uint8[kPersonIndex];
  int8_t no_person_score = output->data.uint8[kNotAPersonIndex];

  float person_score_f =
      (person_score - output->params.zero_point) * output->params.scale;
  float no_person_score_f =
      (no_person_score - output->params.zero_point) * output->params.scale;

  // Respond to detection
  RespondToDetection(person_score_f, no_person_score_f);
  vTaskDelay(1); // to avoid watchdog trigger
}
#endif

#if defined(COLLECT_CPU_STATS)
  long long total_time = 0;
  long long start_time = 0;
  extern long long softmax_total_time;
  extern long long dc_total_time;
  extern long long conv_total_time;
  extern long long fc_total_time;
  extern long long pooling_total_time;
  extern long long add_total_time;
  extern long long mul_total_time;
#endif

void run_inference(void *ptr) {
  /* Convert from uint8 picture data to int8 */
  enable_instruction_counter();
  // Variables para los contadores de ciclos e instrucciones
  unsigned ccount_start, ccount_end, icount_start, icount_end;
  
  RSR(CCOUNT, ccount_start); // Lee el contador de ciclos al inicio de la cuantización
  WSR(ICOUNT, 0);            // Reinicia el contador de instrucciones antes de la cuantización
  // start quantize timer
  long long start_quantize = esp_timer_get_time();

  // Convertir de uint8 a int8 (cuantización de la imagen)
  for (int i = 0; i < kNumCols * kNumRows; i++) {
    input->data.int8[i] = ((uint8_t *) ptr)[i] ^ 0x80;
    // printf("%d, ", input->data.int8[i]);
  }
  // stop quantize timer
  long long end_quantize = esp_timer_get_time();
  long long quantize_time = end_quantize - start_quantize;
  // printf("Image quantization time = %lld microseconds\n", quantize_time);

  RSR(CCOUNT, ccount_end); // Lee el contador de ciclos al final de la cuantización
  RSR(ICOUNT, icount_end); // Lee el contador de instrucciones al final de la cuantización

  unsigned quantize_cycles = ccount_end - ccount_start;
  unsigned quantize_instructions = icount_end;
  float quantize_cpi = (float)quantize_cycles / (float)quantize_instructions;
  printf("Quantization Cycles = %u\n", quantize_cycles);
  printf("Quantization Instrucciones = %u\n", quantize_instructions);
  printf("Quantization CPI = %f\n", quantize_cpi);

  // Resetear contadores para la próxima medición
  RSR(CCOUNT, ccount_start);
  WSR(ICOUNT, 0);

#if defined(COLLECT_CPU_STATS)
  long long start_time = esp_timer_get_time();
#endif
  // Run the model on this input and make sure it succeeds.

  if (kTfLiteOk != interpreter->Invoke()) {
    MicroPrintf("Invoke failed.");
  }

  RSR(CCOUNT, ccount_end);
  RSR(ICOUNT, icount_end);

  unsigned invoke_cycles = ccount_end - ccount_start;
  unsigned invoke_instructions = icount_end;
  float invoke_cpi = (float)invoke_cycles / (float)invoke_instructions;
  printf("Invoke Cycles = %u\n", invoke_cycles);
  printf("Invoke Instrucciones = %u\n", invoke_instructions);
  printf("Invoke CPI = %f\n", invoke_cpi);
  

  TfLiteTensor* output = interpreter->output(0);

  RSR(CCOUNT, ccount_start);
  WSR(ICOUNT, 0);
  // Process the inference results.
  long long start_process_response = esp_timer_get_time();
  int8_t person_score = output->data.uint8[kPersonIndex];
  int8_t no_person_score = output->data.uint8[kNotAPersonIndex];

  float person_score_f =
      (person_score - output->params.zero_point) * output->params.scale;
  float no_person_score_f =
      (no_person_score - output->params.zero_point) * output->params.scale;

  int person_score_int = (person_score_f) * 100 + 30.5;
  int no_person_score_int = (no_person_score_f) * 100 + 30.5;

  printf("\n");
  printf("person score = %d%%\n", person_score_int);
  printf("Not person score = %d%%\n", no_person_score_int);
  printf("\n");
  
  //RespondToDetection(person_score_f, no_person_score_f);

  long long end_process_response = esp_timer_get_time();
  long long process_response_time = end_process_response - start_process_response;
  // printf("Response processing time = %lld microseconds\n", process_response_time);

  RSR(CCOUNT, ccount_end);
  RSR(ICOUNT, icount_end);
  
  unsigned response_cycles = ccount_end - ccount_start;
  unsigned response_instructions = icount_end;
  float response_cpi = (float)response_cycles / (float)response_instructions;
  printf("Response Processing Cycles = %u\n", response_cycles);
  printf("Response Processing Instrucciones = %u\n", response_instructions);
  printf("Response Processing CPI = %f\n", response_cpi);

  unsigned long long total_cycles = quantize_cycles + invoke_cycles + response_cycles;
  unsigned long long total_instructions = quantize_instructions + invoke_instructions + response_instructions;
  float average_cpi = (float)total_cycles / (float)total_instructions;
  printf("Cycles Total= %llu\n", total_cycles);
  printf("Instrucciones Total= %llu\n", total_instructions);
  printf("CPI Promedio del Proyecto = %f\n", average_cpi);

#if defined(COLLECT_CPU_STATS)
  long long total_time = (esp_timer_get_time() - start_time);
  // printf("Total time = %lld microseconds\n", total_time);
  // printf("Softmax time = %lld microseconds\n", softmax_total_time);
  // printf("FullyConnected time = %lld microseconds\n", fc_total_time);
  // printf("DepthConv time = %lld microseconds\n", dc_total_time);
  // printf("Conv time = %lld microseconds\n", conv_total_time);
  // printf("Pooling time = %lld microseconds\n", pooling_total_time);
  // printf("add time = %lld microseconds\n", add_total_time);
  // printf("mul time = %lld microseconds\n", mul_total_time);

  long long layers_time = softmax_total_time + dc_total_time + conv_total_time + fc_total_time + pooling_total_time;
  // printf("layers time = %lld microseconds\n", layers_time);
  long long total_measured_time = quantize_time + layers_time + process_response_time;
  // printf("Total measured time = %lld microseconds\n", total_measured_time);
  // printf("Difference between total time and sum of subtasks = %lld microseconds\n", total_time - total_measured_time);

  /* Reset times */
  total_time = 0;
  softmax_total_time = 0;
  dc_total_time = 0;
  conv_total_time = 0;
  fc_total_time = 0;
  pooling_total_time = 0;
  add_total_time = 0;
  mul_total_time = 0;

#endif
}
