// XCloud360 stage 4 — offline stand-in for the xCloud media server, built on
// pion/webrtc (MIT). Plays the SERVER side of what the Xbox talks to:
//   - answers our offer with a=setup:passive (DTLS server, like xCloud),
//     ICE controlled, UDP4 only;
//   - accepts the four datachannels we open (control/input/message/chat);
//   - "message": replies HandshakeAck to Handshake, then waits for the
//     clientdevicecapabilities startup message before sending ANY video
//     (xCloud starts RTP only after the startup sequence);
//   - streams an Annex-B H.264 file (looped) as RTP (pion payloader: STAP-A
//     SPS+PPS, single NAL, FU-A) + synthetic Opus packets every 20 ms;
//   - jumps to the next IDR on RTCP PLI or "videoKeyframeRequested";
//   - NACK responder (pion default interceptors) retransmits lost packets;
//   - validates every "input" report (length, report type, contiguous
//     sequence numbers) and echoes "ping N" -> "pong N" on "chat";
//   - counts RTCP PLI / NACK / REMB / RR from the receiver.
// Signaling by files in -dir (see e2e_recv.c). Results -> pion.json,
// sent.auidx (one line per AU: <sha1 of NAL list> <idr>), sent.opus.idx.
package main

import (
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/interceptor"
	"github.com/pion/rtcp"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

type stats struct {
	mu                 sync.Mutex
	State              string         `json:"final_state"`
	Channels           []string       `json:"channels"`
	HandshakeSeen      bool           `json:"handshake_seen"`
	StartupTargets     []string       `json:"startup_targets"`
	ControlMessages    map[string]int `json:"control_messages"`
	InputPackets       int            `json:"input_packets"`
	InputMetadata      int            `json:"input_metadata"`
	InputBadLen        int            `json:"input_bad_len"`
	InputSeqGaps       int            `json:"input_seq_gaps"`
	InputFirstPadSeq   uint32         `json:"input_first_pad_seq"`
	InputLastSeq       uint32         `json:"input_last_seq"`
	InputDistinctA     int            `json:"input_a_toggles"`
	InputMaxLX         int            `json:"input_max_lx"`
	InputMinLX         int            `json:"input_min_lx"`
	InputLastElapsedMs float64        `json:"input_last_elapsed_ms"`
	InputPadReports    map[string]int `json:"input_pad_reports"`   // stage 5: reports per gamepad index
	PadsAnnounced      []int          `json:"gamepad_changed_idx"` // stage 5: gamepadChanged(i, added) seen
	PadAnnouncedBefore bool           `json:"pad_announced_before_reports"`
	ChatPings          int            `json:"chat_pings"`
	VideoAUsSent       int            `json:"video_aus_sent"`
	VideoBytesSent     int            `json:"video_bytes_sent"`
	AudioSent          int            `json:"audio_sent"`
	KeyframeJumps      int            `json:"keyframe_jumps"`
	RtcpPLI            int            `json:"rtcp_pli"`
	RtcpNackPkts       int            `json:"rtcp_nack_packets"`
	RtcpNackSeqs       int            `json:"rtcp_nack_seqs"`
	RtcpREMB           int            `json:"rtcp_remb"`
	RembBps            float64        `json:"remb_bps_last"`
	RtcpRR             int            `json:"rtcp_rr"`
	RRFractionLostMax  int            `json:"rr_fraction_lost_max"`
	RRTotalLostLast    uint32         `json:"rr_total_lost_last"`
	RRSSRCMatches      bool           `json:"rr_ssrc_matches_video"`
	ConnectedAfterMs   int64          `json:"connected_after_ms"`
}

var st = stats{ControlMessages: map[string]int{}, InputMinLX: math.MaxInt32, InputMaxLX: math.MinInt32,
	InputPadReports: map[string]int{}, PadAnnouncedBefore: true}

func waitFile(p string, timeout time.Duration) []byte {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if b, err := os.ReadFile(p); err == nil {
			return b
		}
		time.Sleep(10 * time.Millisecond)
	}
	log.Fatalf("timeout waiting for %s", p)
	return nil
}

func writeAtomic(p string, b []byte) {
	if err := os.WriteFile(p+".tmp", b, 0o644); err != nil {
		log.Fatal(err)
	}
	if err := os.Rename(p+".tmp", p); err != nil {
		log.Fatal(err)
	}
}

// ---- H.264 Annex-B -> access units -----------------------------------------

func splitNALs(b []byte) [][]byte {
	var out [][]byte
	i, start := 0, -1
	for i+3 <= len(b) {
		if b[i] == 0 && b[i+1] == 0 && b[i+2] == 1 {
			if start >= 0 {
				end := i
				for end > start && b[end-1] == 0 {
					end--
				}
				out = append(out, b[start:end])
			}
			i += 3
			start = i
			continue
		}
		i++
	}
	if start >= 0 && start < len(b) {
		out = append(out, b[start:])
	}
	return out
}

type au struct {
	nals [][]byte
	idr  bool
}

func accessUnits(nals [][]byte) []au {
	var out []au
	var cur au
	hasSlice := false
	flush := func() {
		if len(cur.nals) > 0 {
			out = append(out, cur)
		}
		cur = au{}
		hasSlice = false
	}
	for _, n := range nals {
		if len(n) == 0 {
			continue
		}
		t := n[0] & 0x1f
		switch {
		case t == 7 || t == 8 || t == 9 || t == 6:
			if hasSlice {
				flush()
			}
		case t == 1 || t == 5:
			if hasSlice && len(n) > 1 && n[1]&0x80 != 0 { // first_mb_in_slice == 0
				flush()
			}
			hasSlice = true
			if t == 5 {
				cur.idr = true
			}
		}
		cur.nals = append(cur.nals, n)
	}
	flush()
	return out
}

// hash of the NAL list as the receiver will see it (pion drops AUD/filler)
func auHash(nals [][]byte) string {
	h := sha1.New()
	for _, n := range nals {
		t := n[0] & 0x1f
		if t == 9 || t == 12 {
			continue
		}
		var l [4]byte
		binary.BigEndian.PutUint32(l[:], uint32(len(n)))
		h.Write(l[:])
		h.Write(n)
	}
	return hex.EncodeToString(h.Sum(nil))
}

func annexB(nals [][]byte) []byte {
	var b bytes.Buffer
	for _, n := range nals {
		b.Write([]byte{0, 0, 0, 1})
		b.Write(n)
	}
	return b.Bytes()
}

func main() {
	dir := flag.String("dir", ".", "signaling/results directory")
	h264 := flag.String("h264", "", "Annex-B H.264 file to stream (looped)")
	fps := flag.Int("fps", 30, "frame rate to pace the stream at")
	seconds := flag.Int("seconds", 20, "max run time")
	flag.Parse()
	log.SetFlags(log.Lmicroseconds)
	log.SetPrefix("[pion] ")

	raw, err := os.ReadFile(*h264)
	if err != nil {
		log.Fatal(err)
	}
	aus := accessUnits(splitNALs(raw))
	nIDR := 0
	for _, a := range aus {
		if a.idr {
			nIDR++
		}
	}
	log.Printf("%s: %d access units, %d IDR", *h264, len(aus), nIDR)

	// ---- API: loopback host candidates, no mDNS, UDP4, default interceptors
	se := webrtc.SettingEngine{}
	se.SetIncludeLoopbackCandidate(true)
	se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled)
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	// xCloud answers a=setup:passive (DTLS server); pion would echo "active"
	if err := se.SetAnsweringDTLSRole(webrtc.DTLSRoleServer); err != nil {
		log.Fatal(err)
	}
	m := &webrtc.MediaEngine{}
	if err := m.RegisterDefaultCodecs(); err != nil {
		log.Fatal(err)
	}
	ir := &interceptor.Registry{}
	if err := webrtc.RegisterDefaultInterceptors(m, ir); err != nil {
		log.Fatal(err)
	}
	api := webrtc.NewAPI(webrtc.WithMediaEngine(m), webrtc.WithInterceptorRegistry(ir), webrtc.WithSettingEngine(se))
	pc, err := api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		log.Fatal(err)
	}
	vt, _ := webrtc.NewTrackLocalStaticSample(webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000,
		SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"}, "video", "xc360")
	at, _ := webrtc.NewTrackLocalStaticSample(webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2}, "audio", "xc360")
	vs, err := pc.AddTrack(vt)
	if err != nil {
		log.Fatal(err)
	}
	if _, err := pc.AddTrack(at); err != nil {
		log.Fatal(err)
	}

	var videoGo atomic.Bool  // startup sequence done
	var kfWanted atomic.Bool // PLI / videoKeyframeRequested
	var t0 = time.Now()

	pc.OnICEConnectionStateChange(func(s webrtc.ICEConnectionState) { log.Printf("ICE %s", s) })
	pc.OnConnectionStateChange(func(s webrtc.PeerConnectionState) {
		log.Printf("PC %s", s)
		st.mu.Lock()
		st.State = s.String()
		if s == webrtc.PeerConnectionStateConnected {
			st.ConnectedAfterMs = time.Since(t0).Milliseconds()
		}
		st.mu.Unlock()
	})

	var lastSeq uint32
	var lastA = -1
	pc.OnDataChannel(func(dc *webrtc.DataChannel) {
		label := dc.Label()
		log.Printf("datachannel %q protocol=%q id=%v ordered=%v", label, dc.Protocol(), *dc.ID(), dc.Ordered())
		st.mu.Lock()
		st.Channels = append(st.Channels, fmt.Sprintf("%s/%s/%d", label, dc.Protocol(), *dc.ID()))
		st.mu.Unlock()
		dc.OnMessage(func(msg webrtc.DataChannelMessage) {
			st.mu.Lock()
			defer st.mu.Unlock()
			switch label {
			case "message":
				s := string(msg.Data)
				if strings.Contains(s, `"type":"Handshake"`) {
					st.HandshakeSeen = true
					dc.SendText(`{"type":"HandshakeAck","version":"messageV1","id":"0ab125e2-6eee-4687-a2f4-5cfb347f0643","cv":"0"}`)
					log.Printf("message: Handshake -> HandshakeAck")
					return
				}
				var w struct {
					Target  string `json:"target"`
					Type    string `json:"type"`
					Content string `json:"content"`
				}
				if json.Unmarshal(msg.Data, &w) == nil && w.Type == "Message" {
					st.StartupTargets = append(st.StartupTargets, w.Target)
					if strings.HasSuffix(w.Target, "clientdevicecapabilities") {
						var caps map[string]any
						_ = json.Unmarshal([]byte(w.Content), &caps)
						log.Printf("message: client caps %v -> starting media", caps)
						videoGo.Store(true)
					}
				}
			case "control":
				var c struct {
					Message      string `json:"message"`
					GamepadIndex int    `json:"gamepadIndex"`
					WasAdded     bool   `json:"wasAdded"`
				}
				_ = json.Unmarshal(msg.Data, &c)
				st.ControlMessages[c.Message]++
				if c.Message == "gamepadChanged" && c.WasAdded {
					st.PadsAnnounced = append(st.PadsAnnounced, c.GamepadIndex)
				}
				if c.Message == "videoKeyframeRequested" {
					kfWanted.Store(true)
				}
			case "input":
				d := msg.Data
				st.InputPackets++
				if len(d) < 6 {
					st.InputBadLen++
					return
				}
				typ := binary.LittleEndian.Uint16(d[0:2])
				seq := binary.LittleEndian.Uint32(d[2:6])
				switch typ {
				case 8:
					st.InputMetadata++
					if len(d) != 15 {
						st.InputBadLen++
					}
					// capture-rtp quirk: metadata from a fresh serializer (seq 1)
					return
				case 2:
					if len(d) != 38 {
						st.InputBadLen++
						return
					}
					if st.InputFirstPadSeq == 0 {
						st.InputFirstPadSeq = seq
					} else if seq != lastSeq+1 {
						st.InputSeqGaps++
					}
					lastSeq = seq
					st.InputLastSeq = seq
					pidx := int(d[15])
					st.InputPadReports[fmt.Sprint(pidx)]++
					if pidx > 0 {
						seen := false
						for _, a := range st.PadsAnnounced {
							if a == pidx {
								seen = true
							}
						}
						if !seen {
							st.PadAnnouncedBefore = false // a report for a pad xCloud was never told about
						}
					}
					st.InputLastElapsedMs = math.Float64frombits(binary.LittleEndian.Uint64(d[6:14]))
					mask := binary.LittleEndian.Uint16(d[16:18])
					a := int(mask>>4) & 1
					if a != lastA {
						st.InputDistinctA++
						lastA = a
					}
					lx := int(int16(binary.LittleEndian.Uint16(d[18:20])))
					if lx > st.InputMaxLX {
						st.InputMaxLX = lx
					}
					if lx < st.InputMinLX {
						st.InputMinLX = lx
					}
				default:
					st.InputBadLen++
				}
			case "chat":
				s := string(msg.Data)
				if strings.HasPrefix(s, "ping ") {
					st.ChatPings++
					dc.SendText("pong " + s[5:])
				}
			}
		})
	})

	// ---- RTCP from the receiver
	go func() {
		for {
			pkts, _, err := vs.ReadRTCP()
			if err != nil {
				return
			}
			st.mu.Lock()
			for _, p := range pkts {
				switch r := p.(type) {
				case *rtcp.PictureLossIndication:
					st.RtcpPLI++
					kfWanted.Store(true)
				case *rtcp.TransportLayerNack:
					st.RtcpNackPkts++
					for _, n := range r.Nacks {
						st.RtcpNackSeqs += len(n.PacketList())
					}
				case *rtcp.ReceiverEstimatedMaximumBitrate:
					st.RtcpREMB++
					st.RembBps = float64(r.Bitrate)
				case *rtcp.ReceiverReport:
					st.RtcpRR++
					for _, rep := range r.Reports {
						if int(rep.FractionLost) > st.RRFractionLostMax {
							st.RRFractionLostMax = int(rep.FractionLost)
						}
						st.RRTotalLostLast = rep.TotalLost
						if len(vs.GetParameters().Encodings) > 0 && uint32(vs.GetParameters().Encodings[0].SSRC) == rep.SSRC {
							st.RRSSRCMatches = true
						}
					}
				}
			}
			st.mu.Unlock()
		}
	}()

	// ---- signaling
	offer := waitFile(filepath.Join(*dir, "offer.sdp"), 60*time.Second)
	if err := pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeOffer, SDP: string(offer)}); err != nil {
		log.Fatalf("SetRemoteDescription: %v", err)
	}
	ans, err := pc.CreateAnswer(nil)
	if err != nil {
		log.Fatal(err)
	}
	gather := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(ans); err != nil {
		log.Fatal(err)
	}
	<-gather
	var sdpLines, cands []string
	for _, l := range strings.Split(pc.LocalDescription().SDP, "\r\n") {
		if strings.HasPrefix(l, "a=candidate:") {
			cands = append(cands, l) // trickled separately (tests xcw_add_remote_candidate)
			continue
		}
		if l == "a=end-of-candidates" {
			continue
		}
		sdpLines = append(sdpLines, l)
	}
	answer := strings.Join(sdpLines, "\r\n")
	if !strings.HasSuffix(answer, "\r\n") {
		answer += "\r\n"
	}
	writeAtomic(filepath.Join(*dir, "answer.sdp"), []byte(answer))
	writeAtomic(filepath.Join(*dir, "remote.cands"), []byte(strings.Join(cands, "\n")+"\n"))
	log.Printf("answer written (%d bytes), %d candidates", len(answer), len(cands))

	// ---- media
	sentIdx, _ := os.Create(filepath.Join(*dir, "sent.auidx"))
	opusIdx, _ := os.Create(filepath.Join(*dir, "sent.opus.idx"))
	deadline := time.Now().Add(time.Duration(*seconds) * time.Second)
	done := filepath.Join(*dir, "recv.done")

	go func() { // Opus: 20 ms packets, deterministic payloads
		tick := time.NewTicker(20 * time.Millisecond)
		defer tick.Stop()
		n := 0
		for range tick.C {
			if !videoGo.Load() {
				continue
			}
			l := 40 + (n*37)%120
			p := make([]byte, l)
			for i := range p {
				p[i] = byte(n*7 + i*13)
			}
			if at.WriteSample(media.Sample{Data: p, Duration: 20 * time.Millisecond}) == nil {
				sum := sha1.Sum(p)
				fmt.Fprintf(opusIdx, "%s\n", hex.EncodeToString(sum[:]))
				st.mu.Lock()
				st.AudioSent++
				st.mu.Unlock()
			}
			n++
		}
	}()

	frame := time.Second / time.Duration(*fps)
	next := time.Now()
	i := 0
	for time.Now().Before(deadline) {
		if _, err := os.Stat(done); err == nil {
			break
		}
		if !videoGo.Load() {
			time.Sleep(5 * time.Millisecond)
			next = time.Now()
			continue
		}
		if kfWanted.Swap(false) {
			for k := 1; k <= len(aus); k++ { // jump to the next IDR
				if aus[(i+k)%len(aus)].idr {
					i = (i + k) % len(aus)
					break
				}
			}
			st.mu.Lock()
			st.KeyframeJumps++
			st.mu.Unlock()
		}
		a := aus[i%len(aus)]
		data := annexB(a.nals)
		if err := vt.WriteSample(media.Sample{Data: data, Duration: frame}); err != nil {
			log.Printf("WriteSample: %v", err)
		}
		idr := 0
		if a.idr {
			idr = 1
		}
		fmt.Fprintf(sentIdx, "%s %d\n", auHash(a.nals), idr)
		st.mu.Lock()
		st.VideoAUsSent++
		st.VideoBytesSent += len(data)
		st.mu.Unlock()
		i = (i + 1) % len(aus)
		next = next.Add(frame)
		if d := time.Until(next); d > 0 {
			time.Sleep(d)
		}
	}
	time.Sleep(300 * time.Millisecond)
	sentIdx.Close()
	opusIdx.Close()
	st.mu.Lock()
	b, _ := json.MarshalIndent(&st, "", "  ")
	st.mu.Unlock()
	writeAtomic(filepath.Join(*dir, "pion.json"), b)
	log.Printf("done: %s", b)
	pc.Close()
}
