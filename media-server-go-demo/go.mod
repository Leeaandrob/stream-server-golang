module github.com/notedit/media-server-go-demo

require (
	github.com/gin-contrib/static v0.0.0-20181225054800-cf5e10bbd933
	github.com/gin-gonic/gin v1.7.0
	github.com/gofrs/uuid v3.1.0+incompatible
	github.com/gorilla/websocket v1.4.0
	github.com/joho/godotenv v1.3.0
	github.com/notedit/gstreamer-go v0.3.0
	github.com/notedit/gstreamer-rtmp v0.0.0-20181226050148-9295bf2f2ca8
	github.com/notedit/media-server-go v0.1.12
	github.com/notedit/media-server-go-demo/rtmp-to-webrtc/rtmpstreamer v0.0.0
	github.com/notedit/rtmp-lib v0.0.1
	github.com/notedit/sdp v0.0.0-20190418080450-702b42591eb2
	github.com/sanity-io/litter v1.1.0
)

require (
	github.com/Jeffail/gabs v1.1.1 // indirect
	github.com/gin-contrib/sse v0.1.0 // indirect
	github.com/go-playground/locales v0.13.0 // indirect
	github.com/go-playground/universal-translator v0.17.0 // indirect
	github.com/go-playground/validator/v10 v10.4.1 // indirect
	github.com/golang/protobuf v1.3.3 // indirect
	github.com/json-iterator/go v1.1.9 // indirect
	github.com/kr/pretty v0.1.0 // indirect
	github.com/leodido/go-urn v1.2.0 // indirect
	github.com/mattn/go-isatty v0.0.12 // indirect
	github.com/modern-go/concurrent v0.0.0-20180306012644-bacd9c7ef1dd // indirect
	github.com/modern-go/reflect2 v1.0.1 // indirect
	github.com/ugorji/go/codec v1.1.7 // indirect
	golang.org/x/crypto v0.0.0-20200622213623-75b288015ac9 // indirect
	golang.org/x/sys v0.0.0-20200116001909-b77594299b42 // indirect
	gopkg.in/check.v1 v1.0.0-20180628173108-788fd7840127 // indirect
	gopkg.in/yaml.v2 v2.2.8 // indirect
)

replace (
	github.com/notedit/media-server-go v0.1.12 => ../media-server-go
	github.com/notedit/media-server-go-demo/rtmp-to-webrtc/rtmpstreamer v0.0.0 => ./rtmp-to-webrtc/rtmpstreamer
)
