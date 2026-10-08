PORTNAME=	xfce4-bsdbluetooth-plugin
DISTVERSIONPREFIX=	v
DISTVERSION=	0.1.1
CATEGORIES=	comms xfce

MAINTAINER=	wugq.dev@gmail.com
COMMENT=	XFCE panel plugin and command line to manage Bluetooth on FreeBSD
WWW=		https://github.com/wugq/xfce4-bsdbluetooth-plugin

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

LIB_DEPENDS=	libnotify.so:devel/libnotify
RUN_DEPENDS=	pkexec:sysutils/polkit \
		ck-list-sessions:sysutils/consolekit2

USES=		gnome pkgconfig xfce
USE_GITHUB=	yes
GH_ACCOUNT=	wugq
USE_GNOME=	glib20 gtk30
USE_XFCE=	panel

MAKE_ARGS=	PREFIX=${PREFIX}
SUB_FILES=	pkg-message

post-install:
	${STRIP_CMD} ${STAGEDIR}${PREFIX}/bin/bsdbt \
		${STAGEDIR}${PREFIX}/libexec/bsdbt-helper \
		${STAGEDIR}${PREFIX}/lib/xfce4/panel/plugins/libbsdbluetooth-plugin.so

.include <bsd.port.mk>
