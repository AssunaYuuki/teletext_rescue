# Minimal 8051 disassembler for the TV-Inform receiver ROM.
import sys

# operand encodings per opcode high/low nibble; we build the standard table.
RNAME = ['R0','R1','R2','R3','R4','R5','R6','R7']

# SFR names for direct addresses
SFR = {0x80:'P0',0x81:'SP',0x82:'DPL',0x83:'DPH',0x87:'PCON',0x88:'TCON',
0x89:'TMOD',0x8A:'TL0',0x8B:'TL1',0x8C:'TH0',0x8D:'TH1',0x90:'P1',0x98:'SCON',
0x99:'SBUF',0xA0:'P2',0xA8:'IE',0xB0:'P3',0xB8:'IP',0xD0:'PSW',0xE0:'ACC',0xF0:'B'}
BITNAME={0x88:'IT0',0x89:'IE0',0x8A:'IT1',0x8B:'IE1',0x8C:'TR0',0x8D:'TF0',
0x8E:'TR1',0x8F:'TF1',0x98:'RI',0x99:'TI',0xAF:'EA',0xAC:'ES',0xA9:'ET0',
0xAA:'ET1',0xAB:'EX1',0xA8:'EX0',0xB8:'PX0',0xD7:'CY',0xD6:'AC',0xD2:'OV'}

def dsfr(a): return SFR.get(a, '%02Xh'%a)
def dbit(a):
    base = a & 0xF8 if a>=0x80 else (0x20 + a//8)
    if a>=0x80:
        return BITNAME.get(a, dsfr(a&0xF8)+'.%d'%(a&7))
    return '%02Xh.%d'%(0x20+a//8, a&7)

def disasm(rom, org=0):
    i=0; out=[]
    def u8(): return rom[i]
    while i < len(rom):
        a=i; op=rom[i]; i+=1
        def b():
            nonlocal i; v=rom[i]; i+=1; return v
        def rel():
            nonlocal i; v=rom[i]; i+=1; return (a+ (i-a) + (v-256 if v>127 else v))
        hi=op>>4; lo=op&0xF
        t=''
        # register ops lo 8..F = Rn, lo 6..7 = @Ri
        def reg(): return RNAME[lo-8] if lo>=8 else ('@R%d'%(lo-6))
        if op==0x00: t='NOP'
        elif op==0x01 or op==0x11 or op==0x21 or op==0x31 or op==0x41 or op==0x51 or op==0x61 or op==0x71 or op==0x81 or op==0x91 or op==0xA1 or op==0xB1 or op==0xC1 or op==0xD1 or op==0xE1 or op==0xF1:
            lo8=b(); addr=((op&0xE0)<<3)|lo8; addr=((a+2)&0xF800)|addr; t='ACALL %04Xh'%addr
        elif op in (0x02,): adr=(b()<<8)|b(); t='LJMP %04Xh'%adr
        elif op==0x12: adr=(b()<<8)|b(); t='LCALL %04Xh'%adr
        elif op==0x22: t='RET'
        elif op==0x32: t='RETI'
        elif op==0x03: t='RR A'
        elif op==0x13: t='RRC A'
        elif op==0x23: t='RL A'
        elif op==0x33: t='RLC A'
        elif op==0x04: t='INC A'
        elif op==0x05: t='INC %s'%dsfr(b())
        elif op in (0x06,0x07): t='INC @R%d'%(op-6)
        elif 0x08<=op<=0x0F: t='INC %s'%RNAME[op-8]
        elif op==0x14: t='DEC A'
        elif op==0x15: t='DEC %s'%dsfr(b())
        elif op in (0x16,0x17): t='DEC @R%d'%(op-0x16)
        elif 0x18<=op<=0x1F: t='DEC %s'%RNAME[op-0x18]
        elif op==0x24: t='ADD A,#%02Xh'%b()
        elif op==0x25: t='ADD A,%s'%dsfr(b())
        elif op in (0x26,0x27): t='ADD A,@R%d'%(op-0x26)
        elif 0x28<=op<=0x2F: t='ADD A,%s'%RNAME[op-0x28]
        elif op==0x34: t='ADDC A,#%02Xh'%b()
        elif op==0x35: t='ADDC A,%s'%dsfr(b())
        elif op in (0x36,0x37): t='ADDC A,@R%d'%(op-0x36)
        elif 0x38<=op<=0x3F: t='ADDC A,%s'%RNAME[op-0x38]
        elif op==0x40: t='JC %04Xh'%rel()
        elif op==0x50: t='JNC %04Xh'%rel()
        elif op==0x60: t='JZ %04Xh'%rel()
        elif op==0x70: t='JNZ %04Xh'%rel()
        elif op==0x42: d=b(); t='ORL %s,A'%dsfr(d)
        elif op==0x43: d=b(); im=b(); t='ORL %s,#%02Xh'%(dsfr(d),im)
        elif op==0x44: t='ORL A,#%02Xh'%b()
        elif op==0x45: t='ORL A,%s'%dsfr(b())
        elif op in (0x46,0x47): t='ORL A,@R%d'%(op-0x46)
        elif 0x48<=op<=0x4F: t='ORL A,%s'%RNAME[op-0x48]
        elif op==0x52: d=b(); t='ANL %s,A'%dsfr(d)
        elif op==0x53: d=b(); im=b(); t='ANL %s,#%02Xh'%(dsfr(d),im)
        elif op==0x54: t='ANL A,#%02Xh'%b()
        elif op==0x55: t='ANL A,%s'%dsfr(b())
        elif op in (0x56,0x57): t='ANL A,@R%d'%(op-0x56)
        elif 0x58<=op<=0x5F: t='ANL A,%s'%RNAME[op-0x58]
        elif op==0x62: d=b(); t='XRL %s,A'%dsfr(d)
        elif op==0x63: d=b(); im=b(); t='XRL %s,#%02Xh'%(dsfr(d),im)
        elif op==0x64: t='XRL A,#%02Xh'%b()
        elif op==0x65: t='XRL A,%s'%dsfr(b())
        elif op in (0x66,0x67): t='XRL A,@R%d'%(op-0x66)
        elif 0x68<=op<=0x6F: t='XRL A,%s'%RNAME[op-0x68]
        elif op==0x72: t='ORL C,%s'%dbit(b())
        elif op==0x82: t='ANL C,%s'%dbit(b())
        elif op==0x92: t='MOV %s,C'%dbit(b())
        elif op==0xA0: t='ORL C,/%s'%dbit(b())
        elif op==0xB0: t='ANL C,/%s'%dbit(b())
        elif op==0xA2: t='MOV C,%s'%dbit(b())
        elif op==0xC2: t='CLR %s'%dbit(b())
        elif op==0xD2: t='SETB %s'%dbit(b())
        elif op==0xB2: t='CPL %s'%dbit(b())
        elif op==0xC3: t='CLR C'
        elif op==0xD3: t='SETB C'
        elif op==0xB3: t='CPL C'
        elif op==0x10: bb=b(); t='JBC %s,%04Xh'%(dbit(bb),rel())
        elif op==0x20: bb=b(); t='JB %s,%04Xh'%(dbit(bb),rel())
        elif op==0x30: bb=b(); t='JNB %s,%04Xh'%(dbit(bb),rel())
        elif op==0x73: t='JMP @A+DPTR'
        elif op==0x74: t='MOV A,#%02Xh'%b()
        elif op==0x75: d=b(); im=b(); t='MOV %s,#%02Xh'%(dsfr(d),im)
        elif op in (0x76,0x77): t='MOV @R%d,#%02Xh'%(op-0x76,b())
        elif 0x78<=op<=0x7F: t='MOV %s,#%02Xh'%(RNAME[op-0x78],b())
        elif op==0x80: t='SJMP %04Xh'%rel()
        elif op==0x83: t='MOVC A,@A+PC'
        elif op==0x93: t='MOVC A,@A+DPTR'
        elif op==0x84: t='DIV AB'
        elif op==0x85: s=b(); d=b(); t='MOV %s,%s'%(dsfr(d),dsfr(s))
        elif op in (0x86,0x87): t='MOV %s,@R%d'%(dsfr(b()),op-0x86)
        elif 0x88<=op<=0x8F: t='MOV %s,%s'%(dsfr(b()),RNAME[op-0x88])
        elif op==0x90: adr=(b()<<8)|b(); t='MOV DPTR,#%04Xh'%adr
        elif op==0x94: t='SUBB A,#%02Xh'%b()
        elif op==0x95: t='SUBB A,%s'%dsfr(b())
        elif op in (0x96,0x97): t='SUBB A,@R%d'%(op-0x96)
        elif 0x98<=op<=0x9F: t='SUBB A,%s'%RNAME[op-0x98]
        elif op==0xA3: t='INC DPTR'
        elif op==0xA4: t='MUL AB'
        elif op==0xA6 or op==0xA7: t='MOV @R%d,%s'%(op-0xA6,dsfr(b()))
        elif 0xA8<=op<=0xAF: t='MOV %s,%s'%(RNAME[op-0xA8],dsfr(b()))
        elif op==0xB4: im=b(); t='CJNE A,#%02Xh,%04Xh'%(im,rel())
        elif op==0xB5: d=b(); t='CJNE A,%s,%04Xh'%(dsfr(d),rel())
        elif op in (0xB6,0xB7): im=b(); t='CJNE @R%d,#%02Xh,%04Xh'%(op-0xB6,im,rel())
        elif 0xB8<=op<=0xBF: im=b(); t='CJNE %s,#%02Xh,%04Xh'%(RNAME[op-0xB8],im,rel())
        elif op==0xC0: t='PUSH %s'%dsfr(b())
        elif op==0xD0: t='POP %s'%dsfr(b())
        elif op==0xC4: t='SWAP A'
        elif op==0xC5: t='XCH A,%s'%dsfr(b())
        elif op in (0xC6,0xC7): t='XCH A,@R%d'%(op-0xC6)
        elif 0xC8<=op<=0xCF: t='XCH A,%s'%RNAME[op-0xC8]
        elif op==0xD4: t='DA A'
        elif op==0xD5: d=b(); t='DJNZ %s,%04Xh'%(dsfr(d),rel())
        elif op in (0xD6,0xD7): t='XCHD A,@R%d'%(op-0xD6)
        elif 0xD8<=op<=0xDF: t='DJNZ %s,%04Xh'%(RNAME[op-0xD8],rel())
        elif op==0xE0: t='MOVX A,@DPTR'
        elif op in (0xE2,0xE3): t='MOVX A,@R%d'%(op-0xE2)
        elif op==0xE4: t='CLR A'
        elif op==0xE5: t='MOV A,%s'%dsfr(b())
        elif op in (0xE6,0xE7): t='MOV A,@R%d'%(op-0xE6)
        elif 0xE8<=op<=0xEF: t='MOV A,%s'%RNAME[op-0xE8]
        elif op==0xF0: t='MOVX @DPTR,A'
        elif op in (0xF2,0xF3): t='MOVX @R%d,A'%(op-0xF2)
        elif op==0xF4: t='CPL A'
        elif op==0xF5: t='MOV %s,A'%dsfr(b())
        elif op in (0xF6,0xF7): t='MOV @R%d,A'%(op-0xF6)
        elif 0xF8<=op<=0xFF: t='MOV %s,A'%RNAME[op-0xF8]
        else: t='DB %02Xh'%op
        out.append((org+a, rom[a:i], t))
    return out

if __name__=='__main__':
    rom=open(sys.argv[1],'rb').read()
    start=int(sys.argv[2],0) if len(sys.argv)>2 else 0
    end=int(sys.argv[3],0) if len(sys.argv)>3 else len(rom)
    for addr,by,txt in disasm(rom[start:end],start):
        print('%04X  %-9s %s'%(addr,by.hex(),txt))
