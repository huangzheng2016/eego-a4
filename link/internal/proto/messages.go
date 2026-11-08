package proto

// TunnelRequest is the head of a central-initiated DATA message.
type TunnelRequest struct {
	M string            `json:"m"`
	P string            `json:"p"`
	H map[string]string `json:"h,omitempty"`
	T int               `json:"t"`
}

// TunnelResponse is the head of the device's DATA reply.
type TunnelResponse struct {
	S int               `json:"s"`
	H map[string]string `json:"h,omitempty"`
}

// ProxyRequest is the head of a device-initiated PROXY message.
type ProxyRequest struct {
	M   string            `json:"m"`
	U   string            `json:"u"`
	H   map[string]string `json:"h,omitempty"`
	T   int               `json:"t"`
	Max int               `json:"max"`
}

// ProxyResponse is the head of the agent's PROXY reply. S=0 means failure (E, Msg set).
type ProxyResponse struct {
	S   int               `json:"s"`
	H   map[string]string `json:"h,omitempty"`
	E   string            `json:"e,omitempty"`
	Msg string            `json:"m,omitempty"`
}

// Info is the JSON served by the INFO characteristic.
type Info struct {
	V           int    `json:"v"`
	MTU         int    `json:"mtu"`
	Name        string `json:"name"`
	FW          string `json:"fw"`
	Provisioned bool   `json:"provisioned"`
	Httpd       bool   `json:"httpd"`
	MaxReq      int    `json:"max_req"`
	MaxResp     int    `json:"max_resp"`
}

// Chunk is the frame size for an ATT MTU (20 when unknown).
func Chunk(mtu int) int {
	if mtu <= 23 {
		return 20
	}
	return min(mtu-3, 512)
}
