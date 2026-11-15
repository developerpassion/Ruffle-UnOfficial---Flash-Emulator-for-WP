using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using Windows.Networking;
using Windows.Networking.Connectivity;
using Windows.Networking.Sockets;
using Windows.Storage;

// Chỉ bản Silverlight 8.0: server HTTP tối giản để nhận .swf từ trình duyệt trên máy khác cùng Wi-Fi.
// GET /           -> trang upload (chọn nhiều file, gửi lần lượt bằng XHR)
// POST /upload?name=<tên>  thân = nội dung file (không multipart) -> lưu vào thư mục đích
namespace RuffleWP80
{
    public class WifiReceiver
    {
        public const int Port = 8080;
        private const long MaxSize = 200L * 1024 * 1024;

        private StreamSocketListener listener;
        private readonly Func<Task<StorageFolder>> target;

        public event Action<string> Received; // tên file đã lưu (gọi trên luồng nền)
        public event Action<string> Error;
        public event Action<string, long, long> Progress; // tên, đã nhận, tổng (gọi trên luồng nền, ~5 lần/giây)

        public WifiReceiver(Func<Task<StorageFolder>> target) { this.target = target; }

        public bool Running { get { return listener != null; } }

        public async Task Start()
        {
            if (listener != null) return;
            var l = new StreamSocketListener();
            l.ConnectionReceived += OnConnection;
            await l.BindServiceNameAsync(Port.ToString());
            listener = l;
        }

        public void Stop()
        {
            if (listener == null) return;
            listener.Dispose();
            listener = null;
        }

        public static List<string> Addresses()
        {
            var list = new List<string>();
            foreach (HostName h in NetworkInformation.GetHostNames())
                if (h.Type == HostNameType.Ipv4 && !h.CanonicalName.StartsWith("127."))
                    list.Add("http://" + h.CanonicalName + ":" + Port + "/");
            return list;
        }

        private async void OnConnection(StreamSocketListener sender, StreamSocketListenerConnectionReceivedEventArgs args)
        {
            using (StreamSocket socket = args.Socket)
            {
                try
                {
                    await Handle(socket.InputStream.AsStreamForRead(), socket.OutputStream.AsStreamForWrite());
                }
                catch (Exception ex)
                {
                    if (Error != null) Error(ex.Message);
                }
            }
        }

        private async Task Handle(Stream input, Stream output)
        {
            // Đọc phần header tới \r\n\r\n
            var head = new MemoryStream();
            var one = new byte[1];
            uint last4 = 0;
            while (last4 != 0x0D0A0D0A)
            {
                if (await input.ReadAsync(one, 0, 1) == 0) return;
                head.WriteByte(one[0]);
                last4 = (last4 << 8) | one[0];
                if (head.Length > 16384) { await Respond(output, "431 Request Header Fields Too Large", "text/plain", "Header too large"); return; }
            }
            string[] lines = Encoding.UTF8.GetString(head.ToArray(), 0, (int)head.Length).Split(new[] { "\r\n" }, StringSplitOptions.None);
            string[] reqLine = lines[0].Split(' ');
            if (reqLine.Length < 2) return;
            string method = reqLine[0].ToUpperInvariant();
            string path = reqLine[1];
            long length = -1;
            foreach (string line in lines)
            {
                int c = line.IndexOf(':');
                if (c > 0 && line.Substring(0, c).Trim().Equals("Content-Length", StringComparison.OrdinalIgnoreCase))
                    long.TryParse(line.Substring(c + 1).Trim(), out length);
            }

            if (method == "GET" && (path == "/" || path.StartsWith("/?")))
            {
                await Respond(output, "200 OK", "text/html; charset=utf-8", Page);
                return;
            }
            if (method == "POST" && path.StartsWith("/upload"))
            {
                if (length < 0) { await Respond(output, "411 Length Required", "text/plain", "Length required"); return; }
                if (length > MaxSize) { await Respond(output, "413 Payload Too Large", "text/plain", "File too large"); return; }
                string name = "upload.swf";
                int q = path.IndexOf("name=");
                if (q >= 0)
                {
                    string v = path.Substring(q + 5);
                    int amp = v.IndexOf('&');
                    if (amp >= 0) v = v.Substring(0, amp);
                    name = Uri.UnescapeDataString(v.Replace('+', ' '));
                }
                name = MainPage.SafeName(Path.GetFileName(name));

                StorageFolder dir = await target();
                StorageFile file = await dir.CreateFileAsync(name, CreationCollisionOption.ReplaceExisting);
                long left = length;
                using (Stream fs = await file.OpenStreamForWriteAsync())
                {
                    var buf = new byte[65536];
                    DateTime lastReport = DateTime.MinValue;
                    if (Progress != null) Progress(name, 0, length);
                    while (left > 0)
                    {
                        int n = await input.ReadAsync(buf, 0, (int)Math.Min(buf.Length, left));
                        if (n == 0) break;
                        await fs.WriteAsync(buf, 0, n);
                        left -= n;
                        if (Progress != null && (left == 0 || (DateTime.UtcNow - lastReport).TotalMilliseconds > 200))
                        {
                            lastReport = DateTime.UtcNow;
                            Progress(name, length - left, length);
                        }
                    }
                }
                if (left > 0)
                {
                    await file.DeleteAsync();
                    if (Error != null) Error(name + ": connection lost");
                    return; // kết nối đứt giữa chừng
                }
                await Respond(output, "200 OK", "text/plain", "OK " + name);
                if (Received != null) Received(name);
                return;
            }
            await Respond(output, "404 Not Found", "text/plain", "Not found");
        }

        private static async Task Respond(Stream output, string status, string type, string body)
        {
            byte[] b = Encoding.UTF8.GetBytes(body);
            byte[] h = Encoding.UTF8.GetBytes("HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                "\r\nContent-Length: " + b.Length + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
            await output.WriteAsync(h, 0, h.Length);
            await output.WriteAsync(b, 0, b.Length);
            await output.FlushAsync();
        }

        private const string Page = @"<!doctype html>
<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Ruffle - send SWF</title>
<style>
body{font-family:Segoe UI,Arial,sans-serif;background:#1e2a44;color:#fff;margin:0;padding:24px;max-width:640px}
h1{font-weight:300;margin:0 0 8px}p{color:#9aa8c8}
.box{border:2px dashed #9aa8c8;padding:24px;text-align:center;margin:16px 0}
button{background:#ffc857;border:0;padding:10px 20px;font-size:16px;cursor:pointer}
li{margin:4px 0}.ok{color:#7dff9a}.err{color:#ff7d7d}
</style></head><body>
<h1>Send SWF to Ruffle</h1>
<p>Choose or drop .swf files. They appear on the phone under 'Opened from email / browser'. Keep Ruffle open on the phone while sending.</p>
<div class='box' id='drop'><input type='file' id='f' accept='.swf' multiple><br><br><button id='go'>Upload</button></div>
<ul id='log'></ul>
<script>
var log=document.getElementById('log'),input=document.getElementById('f');
function row(t,c){var li=document.createElement('li');li.textContent=t;if(c)li.className=c;log.appendChild(li);return li;}
function send(files,i){
 if(i>=files.length)return;
 var file=files[i],li=row(file.name+' ...'),x=new XMLHttpRequest();
 x.open('POST','/upload?name='+encodeURIComponent(file.name));
 if(x.upload)x.upload.onprogress=function(e){if(e.lengthComputable)li.textContent=file.name+' '+Math.round(e.loaded*100/e.total)+'%';};
 x.onload=function(){li.textContent=file.name+(x.status==200?' - sent':' - error '+x.status);li.className=x.status==200?'ok':'err';send(files,i+1);};
 x.onerror=function(){li.textContent=file.name+' - failed (is Ruffle still open?)';li.className='err';send(files,i+1);};
 x.send(file);
}
document.getElementById('go').onclick=function(){if(input.files.length)send(input.files,0);};
var d=document.getElementById('drop');
d.ondragover=function(e){e.preventDefault();};
d.ondrop=function(e){e.preventDefault();send(e.dataTransfer.files,0);};
</script></body></html>";
    }
}
