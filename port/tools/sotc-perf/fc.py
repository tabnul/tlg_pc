import re,sys
for f in sys.argv[1:]:
    rows=[]
    for l in open(r'F:\development\sotc-oussama\out\routes\%s.log'%f,errors='ignore'):
        m=re.match(r'SOTC FPS ([\d.]+) at (\d+)s; (\d+) cmds/frame; readbacks (\d+)/s.*?(?:begin/gpu/finish (\d+)/(\d+)/(\d+))?',l)
        if m and int(m.group(2))>=25: rows.append(m.groups())
    fps=[float(r[0]) for r in rows]; cmd=[int(r[2]) for r in rows]
    heavy=[float(r[0]) for r in rows if int(r[2])>9000]; mid=[float(r[0]) for r in rows if 5000<int(r[2])<=9000]
    g=[int(r[5])/max(1,int(r[3])) for r in rows if r[5]]
    print(f,'mean %.1f  heavy(>9k) %s  mid(5-9k) %.1f  gpu ms/rb %.2f'%(sum(fps)/len(fps),' '.join('%.1f'%x for x in heavy),sum(mid)/max(1,len(mid)),sum(g)/max(1,len(g))))
