# Connecting the serial adapter

RAW-TURN talks to the PC through a USB serial adapter on the SAROO board. The
adapter stays connected while you play.

![Wiring of the USB serial adapter to J2 on the SAROO](j2_wiring.svg)

## What you need

- A USB serial adapter that can run at 3.3 V, for example one with a CH340G.
  Most of them have a jumper or switch for 3.3 V or 5 V.
- Three jumper wires
- Optionally a five-pin header to solder into J2, so the wires can be plugged
  in and out

## Finding J2

Take the SAROO out of its shell. On the right edge of the board, next to the
STM32 chip, there is a row of five pads labelled `G C D T R` from top to
bottom, marked J2. R is the only square pad. The ten-pin header further up is
J3; it is not needed.

## Wiring

1. Set the adapter to **3.3 V**, the voltage the SAROO works with.
2. Connect the adapter's **GND** to pad **G**.
3. Connect the adapter's **RXD** to pad **T**. T is where the SAROO sends.
4. Connect the adapter's **TXD** to pad **R**. R is where the SAROO receives.
5. Leave C and D free, and leave the adapter's VCC unconnected. The Saturn
   powers the SAROO.

Send and receive cross over: what one side sends, the other receives. If the
program finds the adapter but never hears the SAROO, T and R are most likely
swapped.

## Checking it

Put the SAROO back into the Saturn, plug the adapter into the PC and switch
the Saturn on. Start the program and then a game. When the game's title
appears in the program window, the line from the SAROO to the PC works. When
the log says that the SAROO accepted the hook set, the line back works too.
