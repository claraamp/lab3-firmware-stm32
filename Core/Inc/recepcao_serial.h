/*
 * recepcao_serial.h -- o modo de recepcao de segmentos pela serial, na placa
 *
 * A parte com a HAL da recepcao (decisao de 29/10, README "Recepcao de segmentos
 * pela serial"): a USART1 com o DMA2 Stream2 circular na RX, o pedido de entrada
 * (RECEBER) durante a demonstracao e o laco do modo, que entrega os bytes ao
 * parser (recepcao.h) e responde com $SEG de origem S ou $ERR, e $PED.
 *
 * No main.c, tres chamadas, dentro dos blocos USER CODE:
 *
 *   recepcao_serial_escutar();                   // depois das verificacoes do boot
 *   while (1) {
 *     if (recepcao_serial_pedido()) {            // entre um segmento e outro da demo
 *       recepcao_serial_executar(tudo_ok);       // so volta se tudo_ok == 0
 *     }
 *     ...demonstracao...
 *   }
 *
 * O DMA so le a UART; o envio continua pelo HAL_UART_Transmit bloqueante. Um erro
 * de linha (ruido, quadro, overrun) faz a HAL abortar a recepcao por DMA: o
 * HAL_UART_ErrorCallback() daqui marca, e o laco arma o DMA de novo.
 */

#ifndef RECEPCAO_SERIAL_H
#define RECEPCAO_SERIAL_H

/* Arma um DMA circular pequeno (64 B) que guarda o que chega pela UART durante a
 * demonstracao, para o recepcao_serial_pedido() procurar o RECEBER. Sem isso, os
 * bytes que chegam durante os HAL_Delay da demonstracao dariam overrun. */
void recepcao_serial_escutar(void);

/* 1 se chegou a linha "RECEBER" (terminada em \n ou \r) desde a ultima chamada.
 * Outras linhas sao ignoradas. */
int recepcao_serial_pedido(void);

/* O modo de recepcao. Com `ok` (as verificacoes do boot passaram): manda as linhas
 * ref dos 10 clipes e o $CAB, e entra no laco de pedidos, de onde so sai por reset.
 * Sem `ok`: manda uma linha de recusa e volta (a demonstracao continua piscando o
 * erro). */
void recepcao_serial_executar(int ok);

#endif /* RECEPCAO_SERIAL_H */
