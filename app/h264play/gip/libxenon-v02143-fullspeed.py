p = "ehci.c"
s = open(p).read()
def sub(old, new):
    global s
    assert s.count(old) == 1, old[:70]
    s = s.replace(old, new)
sub("""		status = ehci_readl(status_reg);
		if ((PORT_OWNER & status) || !(PORT_CONNECT & status) || !(status & PORT_PE) || !(status & PORT_POWER)) {
			/*ehci_writel( 0x1803,status_reg);""", """		status = ehci_readl(status_reg);
		/* orbita v0.21.43: conectado e a porta não habilitou depois do reset =
		 * dispositivo full-speed (controle com fio, receptor, adaptador). A regra
		 * do EHCI é entregar pro OHCI (PORT_OWNER). Antes tentava 4 vezes e o
		 * ehci_reset_port2 ainda cortava a energia da porta: o controle caía e
		 * voltava no meio da enumeração do OHCI e o boot podia travar. */
		if (!(PORT_OWNER & status) && (PORT_CONNECT & status) && (status & PORT_POWER) && !(status & PORT_PE)) {
			ehci_writel((status & ~PORT_RWC_BITS) | PORT_OWNER, status_reg);
			usblog("ehci port %d: full-speed device (status %08x), handing it to OHCI", port, (unsigned)status);
			handshake_mode = f;
			return -ENODEV;
		}
		if ((PORT_OWNER & status) || !(PORT_CONNECT & status) || !(status & PORT_PE) || !(status & PORT_POWER)) {
			/*ehci_writel( 0x1803,status_reg);""")
sub("""	int ret = ehci_reset_port_old(ehci, port);
	if (ret < 0/*==-ENODEV || ret==-ETIMEDOUT*/) {
		u32 status = ehci_readl(status_reg);
		if (status & 1) {""", """	int ret = ehci_reset_port_old(ehci, port);
	if (ret < 0/*==-ENODEV || ret==-ETIMEDOUT*/) {
		u32 status = ehci_readl(status_reg);
		if ((status & 1) && !(status & PORT_OWNER)) {   /* orbita v0.21.43: entregue ao OHCI = não corta a energia */""")
open(p, "w").write(s)
print("ok")
def sub(p, old, new):
    s = open(p).read()
    assert s.count(old) == 1, (p, old[:70])
    open(p, "w").write(s.replace(old, new))
sub("ehci.c", """	retval = ehci_reset_usb_port(ehci, port);
	if (retval >= 0) retval = ehci_init_port(ehci, port);
	return retval;""", """	retval = ehci_reset_usb_port(ehci, port);
	/* orbita v0.21.43: soltou pro OHCI (full/low speed): não é nosso, nada de
	 * GET_DESCRIPTOR numa porta que o EHCI não tem mais */
	if (retval >= 0 && (ehci_readl(&ehci->regs->port_status[port]) & PORT_OWNER)) return -ENODEV;
	if (retval >= 0) retval = ehci_init_port(ehci, port);
	return retval;""")
sub("usbstorage.c", """				if (status & 1) {
					if (i < 16 && ++usb_port_fail[j][i] > USB_PORT_GIVEUP) continue;   /* orbita v0.21.41 */""",
"""				if ((status & 1) && (status & PORT_OWNER)) continue;   /* orbita v0.21.43: é do OHCI (controle, receptor) */
				if (status & 1) {
					if (i < 16 && ++usb_port_fail[j][i] > USB_PORT_GIVEUP) continue;   /* orbita v0.21.41 */""")
sub("usbstorage.c", """					if (ehci_reset_port2(ehci, i) < 0) {
						ehci_msleep(100);
						ehci_reset_port(ehci, i);
					}""", """					/* orbita v0.21.43: se o reset entregou a porta pro OHCI, o
					 * ehci_reset_port tomava ela de volta (o controle caía) */
					if (ehci_reset_port2(ehci, i) < 0 && !(ehci_readl(&ehci->regs->port_status[i]) & PORT_OWNER)) {
						ehci_msleep(100);
						ehci_reset_port(ehci, i);
					}""")
print("ok")
