<CsoundSynthesizer>
<CsOptions>
csound -RWdo temp.wav temp.orc temp.sco
</CsOptions>
<CsInstruments>
sr = 44100
kr = 4410
ksmps = 10
nchnls = 2

gihandle1 vstinit "c:/projects/music/library/polyiblit.dll", 1

instr 1
ichannel = p1
ikey = p4
ivelocity = p5
vstout gihandle1,144,ichannel, ikey, ivelocity
ain1 = 0
ab1, ab2 vstplug gihandle1, ain1, ain1
outs ab1, ab2
endin
</CsInstruments>
<CsScore>
i 1 0 10 60 60
i 1 2 1 72 60
i 1 2 2 64 60
i 1 15 15 60 60
i 1 15 15 64 60
i 1 15 15 62 60


</CsScore>
</CsoundSynthesizer>
