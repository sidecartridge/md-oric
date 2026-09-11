; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .
; bit 30: TOS application .
; bit 31: TTP

ROM4_ADDR			equ $FA0000
CENTERED_XPOS		equ 16 ; Centered X position for Oric low res. 16 bytes (32 pixels) margin on left side
FRAMEBUFFER_A_ADDR	equ (ROM4_ADDR + $1000)
FRAMEBUFFER_B_ADDR	equ (ROM4_ADDR + $8000) ; Upper half of the window, freed from the Oric ROM
AYBUFFER_ADDR		equ (FRAMEBUFFER_A_ADDR + (ORIC_LINES*ORIC_WORDS_PER_LINE*2*3)) ; AY sound buffer after the framebuffer
AYBUFFER_SIZE		equ 512 ; Size of the AY sound buffer in bytes
COPIED_CODE_OFFSET	equ $00010000 ; The offset should be below the screen memory
COPIED_CODE_SIZE	equ $00001000
; Delay between seeing REMOTE_RESET and jumping through the reset vector. The
; RP reboots into Booster right after writing the command, and TOS must not scan
; the cartridge before Booster is back on the bus. $FFFF (~65 ms) is shorter
; than an RP reboot; $FFFFF (~1 s) is what Booster itself uses in the other
; direction, so match it.
PRE_RESET_WAIT		equ $000FFFFF ; Wait this many cycles before resetting the computer
SCREEN_A_BASE_ADDR  equ $60000 ; The screen memory address for the framebuffer
SCREEN_B_BASE_ADDR  equ $70000 ; The screen memory address for the framebuffer
ORIC_LINES		 	equ 224
ORIC_WORDS_PER_LINE equ 15
TIMERB_COUNT_SCAN_LINES		    EQU 10
TIMERB_EVENT_COUNT              EQU 8
LOW_BORDER_OVERSCAN_START       EQU 190

_conterm			equ $484		; Conterm device number
ACIA_BASE 	   		equ $fffffc00   ; Base address of the ACIA

; m68k -> RP signalling goes through ROM3 ($FBxxxx). The RP samples every
; ROM3 read into a DMA ring, so a read is a message and back-to-back reads
; are safe. Values must match EMUL_ROM3_* in rp/src/include/emul.h.
ROM3_ADDR		      	  equ $FB0000
KEY_WINDOW_ADDR		      equ (ROM3_ADDR + $8200)		  ; + IKBD byte: bit 7 = release, low 7 bits = scancode
BLITDONE_ADDR		      equ (ROM3_ADDR + $8400)		  ; Blit finished, RP may reuse the other buffer
ROMCMD_START_ADDR:        equ (ROM4_ADDR + $F000)         ; Legacy: only the unused check_keys macro refers to it
CMD_KEYPRESS		   	  equ ($0BCD) 					  ; Legacy (check_keys)
CMD_KEYRELEASE		      equ ($0CBA) 					  ; Legacy (check_keys)
CMD_BOOSTER		      	  equ ($0DEF) 					  ; Booster command

LISTENER_ADDR		      equ (ROM4_ADDR + $5F8)		  ; RP->m68k command longword, polled once per VBL (past the code, inside the copied $1000)
BOOTSTATUS_ADDR		      equ (ROM4_ADDR + $5FC)		  ; RP->m68k word, read once at startup: 0 = go, 1 = no microSD card
BOOT_NO_SDCARD		      equ $1						  ; The RP could not mount a card, so there is no ROM to run
REMOTE_RESET		      equ $1					      ; The device ask to reset the

AYBUFF_POS		          equ $8                          ; Offset of the AY sound buffer position

; Frame counter published by the RP once per completed frame. A word, free to
; wrap: we only ever test it for inequality against our own saved copy. Sits in
; the last longword of the copied code block, just below the framebuffer.
FRAMECOUNT_ADDR	          equ (ROM4_ADDR + COPIED_CODE_SIZE - 4)

_dskbufp                  equ $4c6                        ; Address of the disk buffer pointer    

; Video base address
VIDEO_BASE_ADDR_LOW     equ $ffff820d
VIDEO_BASE_ADDR_MID     equ $ffff8203
VIDEO_BASE_ADDR_HIGH    equ $ffff8201


	include inc/tos.s

; Macros
; XBIOS Vsync wait
vsync_wait          macro
					move.w #37,-(sp)
					trap #14
					addq.l #2,sp
                    endm    

; XBIOS GetRez
; Return the current screen resolution in D0
get_rez				macro
					move.w #4,-(sp)
					trap #14
					addq.l #2,sp
					endm

; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

; XBIOS Set the screen memory address
set_screen_base	    macro
					move.w #-1, -(sp);  ; No change res
					pea \1 				; Set the physical screen address
					pea \1 				; Set the logical screen address
					move.w #5,-(sp)		; Set the function number to 3 (set screen base)
					trap #14			; Call XBIOS
					lea 12(sp), sp		; Clean the stack
					endm

; Check the keys pressed
check_keys			macro

					move.l #(ROMCMD_START_ADDR), a0 ; Start address of the ROM3

					btst #0, ACIA_BASE
					beq .\@no_key

					move.b (ACIA_BASE + 2), d0	; Read the ACIA status register
					and.w #$FF, d0

					btst #7, d0
					beq.s .\@break_code_key

					move.w #CMD_KEYRELEASE, d7 	 ; Command for key release					
					tst.b (a0, d7.w)             ; Command
					tst.b (a0, d0.w)             ; Key released
					bra.s .\@no_key
.\@break_code_key:
					move.w #CMD_KEYPRESS, d7 	 ; Command for key press
					tst.b (a0, d7.w)             ; Command
					tst.b (a0, d0.w)             ; Key pressed

					; If we are here, no key was pressed
.\@no_key:

					endm

check_commands		macro
					move.l (LISTENER_ADDR), d6	; Store in the D6 register the remote command value
					cmp.l #REMOTE_RESET, d6		; Check if the command is a reset command
					beq .reset
					endm

	section

;Rom cartridge

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l 0
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "ORIC",0
    even

pre_auto:

; Get the resolution of the screen
.get_resolution:
	get_rez
	tst.w d0
	bne lowres_only

; The RP has already tried to mount the microSD card by now. Without one there
; is no Oric ROM to run, so take the same exit as the resolution check: say
; why, and hand back to GEM.
.check_sdcard:
	move.w BOOTSTATUS_ADDR, d0
	cmp.w #BOOT_NO_SDCARD, d0
	beq no_sdcard

start_oric:
; Move the code below the screen memory
	lea (SCREEN_A_BASE_ADDR-COPIED_CODE_OFFSET), a2
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #COPIED_CODE_SIZE, d6
    lea ROM4_ADDR, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6

.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	lea SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (start_rom_code - ROM4_ADDR), a3	; Save the screen memory address in A3
	jmp (a3)

start_rom_code:
; Set colors
 	move.w #$000, $FF8240 	; Set the index 0 color to black
 	move.w #$700, $FF8242 	; Set the index 1 color
 	move.w #$070, $FF8244 	; Set the index 2 color
 	move.w #$770, $FF8246 	; Set the index 3 color
 	move.w #$007, $FF8248 	; Set the index 4 color
 	move.w #$707, $FF824A 	; Set the index 5 color
 	move.w #$077, $FF824C 	; Set the index 6 color
 	move.w #$777, $FF824E 	; Set the index 7 color
 	move.w #$000, $FF8250 	; Set the index 8 color
 	move.w #$700, $FF8252 	; Set the index 9 color
 	move.w #$070, $FF8254 	; Set the index 10 color
 	move.w #$770, $FF8256 	; Set the index 11 color
 	move.w #$007, $FF8258 	; Set the index 12 color
 	move.w #$707, $FF825A 	; Set the index 13 color
 	move.w #$077, $FF825C 	; Set the index 14 color
 	move.w #$777, $FF825E 	; Set the index 15 color

	; Clean the memory screen
	lea SCREEN_A_BASE_ADDR, a0
	lea SCREEN_B_BASE_ADDR, a1
	move.w #7999, d7
.clear_screen_a:
	clr.l (a0)+
	clr.l (a1)+
	dbf d7, .clear_screen_a

	move.w  #$2700,sr			;Interrupts off

	; Set dummy interrupt routines to avoid crashes
	move.l #(SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.dummy - ROM4_ADDR)), d0
	move.l	d0,$68.w			;Install our own HBL (dummy)
	move.l	d0,$134.w			;Install our own Timer A (dummy)
	move.l	d0,$114.w			;Install our own Timer C (dummy)
	move.l	d0,$110.w			;Install our own Timer D (dummy)
	; The keyboard ACIA interrupts through MFP GPIP4 ($118). Reading bytes as
	; they arrive is the only way to never miss one: the 6850 holds a single
	; byte and the IKBD sends a burst at one byte per 1.28 ms, while Timer-B
	; (which used to poll it) counts display-enable pulses and so cannot fire
	; at all during the vertical blank and top border -- ~4 ms every frame.
	move.l #(SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.acia_routine - ROM4_ADDR)),$118.w

	; VBL now is a simple flag set
	move.l #(SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.vblank_routine - ROM4_ADDR)), d0
	move.l	d0,$70.w			;Install our own VBL
	 
	; Disable and mask out all MFP Timers
	clr.b	$fffffa07.w			;Interrupt enable A (Timer-A & B)
	clr.b	$fffffa09.w			;Interrupt enable B (Timer-C & D)
	clr.b	$fffffa13.w			;Interrupt mask A (Timer-A & B)
	clr.b	$fffffa15.w			;Interrupt mask B (Timer-C & D)

	clr.b $fffffa1b.w		   ; Stop Timer B
	move.l #(SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.timerb_routine - ROM4_ADDR)),$120.w ; Timer B interrupt vector
	bset #0,$fffffa07        ; Interrupt Enable for Timer B (1=Enable, 0=Disable)
	bset #0,$fffffa13        ; Interrupt Mask for Timer B (1=Unmask, 0=Mask)

	; Keyboard ACIA: receive interrupt on (TOS's own setting, 8N1 /64), and
	; the MIDI ACIA's receive interrupt off -- both share the GPIP4 line and
	; nothing services MIDI here.
	move.b #$96,$fffffc00.w
	move.b #$15,$fffffc04.w
	bset #6,$fffffa09.w      ; Interrupt Enable B: GPIP4 (ACIA)
	bset #6,$fffffa15.w      ; Interrupt Mask B: GPIP4 (ACIA)
	move.b	#TIMERB_COUNT_SCAN_LINES,$fffffa21.w   		; Timer B data (number of scanlines to next interrupt)
;	bclr #3,$fffffa17.w        ; Set Automatic End-Interrupt
	move.b	#TIMERB_EVENT_COUNT,$fffffa1b.w			    ; Timer B control (event mode (HBL))

	move.w	#$2300,sr		   ;Interrupts back on

	move.b #$12,$fffffc02    ; Turn off mouse reporting (for stability)

	lea (SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.vblank_flag - ROM4_ADDR)), a6
	clr.w (a6)			; Clear VBL flag
	clr.l 2(a6)			; Clear last frame counter (a6+2) and page flag (a6+4)
	clr.w 6(a6)			; Clear overscan flag
	clr.w AYBUFF_POS(a6); Clear AY sound buffer position

	move.l #160, d6	; Bytes to add to complete the full ST line

	clr.b VIDEO_BASE_ADDR_LOW.w			; put in low screen address byte
	clr.w AYBUFF_POS(a6)			; Clear AY sound buffer position


.loop_low_st:

; 	move.w #$500, $FFFF8240.w 	; Set the index 0 color to black

	; Wait for VBL
	move.w #-1, (a6)
.vbl_wait_loop:
	tst.w (a6)
	bne.s .vbl_wait_loop

; 	clr.w $FFFF8240.w 	; Set the index 0 color to black

	; Compute the AY sound buffer position
	lea AYBUFFER_ADDR, a1
.continue_ay_sound:
	move.w AYBUFF_POS(a6), d0
	cmp.w #$FFFF,(a1, d0.w)
	beq.s .no_ay_sound ; No sound to play
	move.b 0(a1, d0.w), $FFFF8800.w ; Set AY register to write
	move.b 1(a1, d0.w), $FFFF8802.w ; Set AY register data
	addq.w #2, d0
	and.w #(AYBUFFER_SIZE - 1), d0 ; Wrap around the buffer
	move.w d0, AYBUFF_POS(a6)
	bra.s .continue_ay_sound

.no_ay_sound:
	; Once per VBL: has the RP asked us to reset the ST? It does this before
	; rebooting itself into Booster, so the machine cold-boots into Booster's
	; cartridge rather than sitting here with our palette and no frames.
	; Inline rather than the check_commands macro: that one uses d6, which in
	; this loop is the 160-byte line stride the blit relies on. d0 is free
	; here -- the frame-counter read below overwrites it anyway.
	move.l (LISTENER_ADDR), d0
	cmp.l #REMOTE_RESET, d0
	beq .reset

	move.w FRAMECOUNT_ADDR, d0	; Frame counter published by the RP
	cmp.w 2(a6), d0
 	beq.s .loop_low_st ; Counter unchanged: no new frame, wait for next VBL

	; Bit 0 of the counter names the buffer the RP just finished writing.
	lea FRAMEBUFFER_A_ADDR, a0
	btst #0, d0
	beq.s .src_chosen
	lea FRAMEBUFFER_B_ADDR, a0
.src_chosen:
	move.w #ORIC_LINES-1, d7		; Number of lines to copy

	move.w d0, 2(a6)	; Remember the counter we are about to blit
	; The destination page is ours to choose: alternate every blit so we never
	; write the page currently being displayed. Deriving it from the RP value
	; used to drop a frame whenever the RP produced two frames in one ST frame.
	tst.w 4(a6)
	bne .fb_b
.fb_a:
	lea (SCREEN_A_BASE_ADDR + CENTERED_XPOS), a1
.copy_planes_a:
	move.l (a0)+, (a1)
	move.w (a0)+, 4(a1)
	move.l (a0)+, 8(a1)
	move.w (a0)+, 12(a1)
	move.l (a0)+, 16(a1)
	move.w (a0)+, 20(a1)
	move.l (a0)+, 24(a1)
	move.w (a0)+, 28(a1)
	move.l (a0)+, 32(a1)
	move.w (a0)+, 36(a1)
	move.l (a0)+, 40(a1)
	move.w (a0)+, 44(a1)
	move.l (a0)+, 48(a1)
	move.w (a0)+, 52(a1)
	move.l (a0)+, 56(a1)
	move.w (a0)+, 60(a1)
	move.l (a0)+, 64(a1)
	move.w (a0)+, 68(a1)
	move.l (a0)+, 72(a1)
	move.w (a0)+, 76(a1)
	move.l (a0)+, 80(a1)
	move.w (a0)+, 84(a1)
	move.l (a0)+, 88(a1)
	move.w (a0)+, 92(a1)
	move.l (a0)+, 96(a1)
	move.w (a0)+, 100(a1)
	move.l (a0)+, 104(a1)
	move.w (a0)+, 108(a1)
	move.l (a0)+, 112(a1)
	move.w (a0)+, 116(a1)

	add.l d6, a1	; Next line
	dbf d7, .copy_planes_a
	move.w #1, 4(a6)	; Next blit targets page B
	move.b  #(SCREEN_A_BASE_ADDR >> 16), VIDEO_BASE_ADDR_HIGH.w           ; put in high screen address byte
	move.b  #((SCREEN_A_BASE_ADDR >> 8) & $ff), VIDEO_BASE_ADDR_MID.w       ; put in mid screen address byte
	tst.b BLITDONE_ADDR	; Tell the RP the blit is done (ROM3 read)
	bra .loop_low_st	; Continue displaying framebuffers in Atari ST mode

.fb_b:
	lea (SCREEN_B_BASE_ADDR + CENTERED_XPOS), a1
.copy_planes_b:
	move.l (a0)+, (a1)
	move.w (a0)+, 4(a1)
	move.l (a0)+, 8(a1)
	move.w (a0)+, 12(a1)
	move.l (a0)+, 16(a1)
	move.w (a0)+, 20(a1)
	move.l (a0)+, 24(a1)
	move.w (a0)+, 28(a1)
	move.l (a0)+, 32(a1)
	move.w (a0)+, 36(a1)
	move.l (a0)+, 40(a1)
	move.w (a0)+, 44(a1)
	move.l (a0)+, 48(a1)
	move.w (a0)+, 52(a1)
	move.l (a0)+, 56(a1)
	move.w (a0)+, 60(a1)
	move.l (a0)+, 64(a1)
	move.w (a0)+, 68(a1)
	move.l (a0)+, 72(a1)
	move.w (a0)+, 76(a1)
	move.l (a0)+, 80(a1)
	move.w (a0)+, 84(a1)
	move.l (a0)+, 88(a1)
	move.w (a0)+, 92(a1)
	move.l (a0)+, 96(a1)
	move.w (a0)+, 100(a1)
	move.l (a0)+, 104(a1)
	move.w (a0)+, 108(a1)
	move.l (a0)+, 112(a1)
	move.w (a0)+, 116(a1)

	add.l d6, a1	; Next line
	dbf d7, .copy_planes_b

	clr.w 4(a6)			; Next blit targets page A
	move.b  #(SCREEN_B_BASE_ADDR >> 16), VIDEO_BASE_ADDR_HIGH.w           ; put in high screen address byte
	move.b  #((SCREEN_B_BASE_ADDR >> 8) & $ff), VIDEO_BASE_ADDR_MID.w       ; put in mid screen address byte
	tst.b BLITDONE_ADDR	; Tell the RP the blit is done (ROM3 read)

	bra .loop_low_st	; Continue displaying framebuffers in Atari ST mode

.timerb_routine:
	sub.w #TIMERB_COUNT_SCAN_LINES, (SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.overscan_flag - ROM4_ADDR))
	beq.s .start_overscan

.no_overscan:
	bclr    #0, $fffffa0f            ; tell ST interrupt is done
.dummy:
	rte

; Keyboard ACIA receive interrupt (MFP GPIP4). One cart read per IKBD byte:
; the byte itself is the address offset and bit 7 already says press or
; release, so there is no command word to pair with. Drains every byte the
; ACIA holds before ending the interrupt, since the line is level-sensitive.
.acia_routine:
	movem.l d0/a0,-(sp)
	lea KEY_WINDOW_ADDR, a0
.acia_next:
	btst #0, ACIA_BASE.w         ; RDRF: a byte is waiting
	beq.s .acia_done
	moveq #0, d0
	move.b (ACIA_BASE + 2).w, d0 ; Read the IKBD byte (clears RDRF)
	tst.b (a0, d0.w)             ; Emit it
	bra.s .acia_next
.acia_done:
	movem.l (sp)+, d0/a0
	bclr    #6, $fffffa11.w      ; ISRB: ACIA interrupt serviced
	rte

.start_overscan:
	clr.b $fffffa1b.w		   ; Stop Timer B
	move.l #(SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.timerb_overscan - ROM4_ADDR)),$120.w ; Timer B interrupt vector
	move.b	#(TIMERB_COUNT_SCAN_LINES - 1),$fffffa21.w  ; Timer B data (number of scanlines to next interrupt)
	move.b	#TIMERB_EVENT_COUNT,$fffffa1b.w			    ; Timer B control (event mode (HBL))	
	; Hold off the keyboard for the ~9 scanlines before the border trick.
	; Every MFP source is level 6, so the 68000 masks Timer B for as long as
	; the ACIA handler runs -- and .timerb_overscan has to hit its cycle
	; exactly or the shifter loses lock and the frame comes out black. We are
	; inside a Timer B interrupt here, so no ACIA handler can be running;
	; masking now guarantees none starts before the trick. Nothing is lost:
	; the 6850 holds a byte, the IKBD sends one per 1.28 ms, and a masked
	; interrupt stays pending in IPRB and fires the moment it is unmasked.
	bclr #6,$fffffa15.w      ; Interrupt Mask B: GPIP4 (ACIA) off
	bra.s .no_overscan

.timerb_overscan:
	rept 112
	nop
	endr

	move.b	#0,$ffff820a.w	; LineCycles=388-416
	rept 24
	nop
	endr
	move.b	#2,$ffff820a.w	; LineCycles=500-508

	; The trick is done, so let the keyboard back in.
	bset #6,$fffffa15.w      ; Interrupt Mask B: GPIP4 (ACIA) on
	; Timer B has nothing left to do this frame -- the VBL handler re-arms
	; it. It used to be left running here to keep polling the keyboard, which
	; the ACIA interrupt now does; leaving it on only added interrupts
	; through the opened border.
	clr.b $fffffa1b.w		   ; Stop Timer B
	bclr    #0, $fffffa0f      ; tell ST interrupt is done
	rte


.vblank_routine:
	clr.b $fffffa1b.w		   ; Stop Timer B
	move.l #(SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.timerb_routine - ROM4_ADDR)),$120.w ; Timer B interrupt vector
	move.b	#TIMERB_COUNT_SCAN_LINES,$fffffa21.w   		; Timer B data (number of scanlines to next interrupt)
	move.b	#TIMERB_EVENT_COUNT,$fffffa1b.w			    ; Timer B control (event mode (HBL))

	clr.w (SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.vblank_flag - ROM4_ADDR))
	move.w #LOW_BORDER_OVERSCAN_START, (SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (.overscan_flag - ROM4_ADDR))
	rte
.vblank_flag:
	dc.w 0						; a6+0
.last_framecount:
	dc.w 0						; a6+2  last RP frame counter we blitted
.page_flag:
	dc.w 0						; a6+4  0 = next blit targets page A, else page B
.overscan_flag:
	dc.w 0						; a6+6
.aybuff_pos:					; AYBUFF_POS equ $8 -> a6+8. Must stay immediately
	dc.w 0						; after .overscan_flag to keep that offset.


; Cold reset. Clearing memvalid/memval2/memval3 makes TOS treat this as a
; power-on: it re-sizes RAM and reinitialises the shifter (palette, base,
; resolution), the MFP, the IKBD and every vector we hijacked. Nothing has to
; be restored by hand first. Runs from the copied block in ST RAM, and touches
; no cartridge address while it waits, so the RP can be gone by then.
.reset:
    move.l #PRE_RESET_WAIT, d6
.wait_me:
    subq.l #1, d6           ; Decrement the outer loop
    bne.s .wait_me          ; Wait for the timeout

	clr.l $420.w			; Invalidate memory system variables
	clr.l $43A.w
	clr.l $51A.w
	move.l $4.w, a0			; Now we can safely jump to the reset vector
	jmp (a0)
	nop
.end_reset_code_in_stack:

lowres_only:
	print lowres_only_txt
    rts

lowres_only_txt: 
	dc.b "Oric Emulator only supports low res",$d,$a
	dc.b 0

	even

no_sdcard:
	print no_sdcard_txt
    rts

no_sdcard_txt:
	dc.b "Oric Emulator: no microSD card.",$d,$a
	dc.b "Insert one and reset.",$d,$a
	dc.b 0

	even


rom_function:
	; Place here your driver code
	rts


end_rom_code:
end_pre_auto:
	even

	dc.l 0
