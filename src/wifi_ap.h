#include <WiFi.h>
#include "settings.h"

const char *ssid = "TWANG_AP";
const char *passphrase = "12345678";

WiFiServer server(80);

char linebuf[80];
int charcount = 0;

enum PAGE_TO_SEND
{
	Stats,
	Metrics
};

void ap_setup()
{
	bool ret;

	/*
	 * Set up an access point
	 * @param ssid          Pointer to the SSID (max 63 char).
	 * @param passphrase    (for WPA2 min 8 char, for open use NULL)
	 * @param channel       WiFi channel number, 1 - 13.
	 * @param ssid_hidden   Network cloaking (0 = broadcast SSID, 1 = hide SSID)
	 */
	// Running SoftAP and station at once costs stability and pins the radio
	// channel to whatever the router picked. With a network already joined the
	// settings page is reachable at the station address instead, so the access
	// point is only raised when there is nothing else.
	if (WiFi.status() == WL_CONNECTED)
	{
		server.begin();
		Serial.print("\r\nWeb Server Address: http://");
		Serial.println(WiFi.localIP());
		return;
	}

	ret = WiFi.softAP(ssid, passphrase, 2, 0);
	server.begin();

	Serial.print("\r\nWiFi SSID: ");
	Serial.println(ssid);
	Serial.print("WiFi Password: ");
	Serial.println(passphrase);
	Serial.println("Web Server Address: http://192.168.4.1");
}

void sendStatsPage(WiFiClient client)
{
	// HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
	// and a content-type so the client knows what's coming, then a blank line:
	client.println("HTTP/1.1 200 OK");
	client.println("Content-type:text/html");
	client.println();
	client.println("<html>");
	client.println("<body>");
	client.println("<h1>TWANG32 Play Stats</h1>");
	client.println("<ul>");

	client.print("<li>Games played: ");
	client.print(user_settings.games_played);
	client.println("</li>");
	if (user_settings.games_played > 0)
	{ // prevent divide by 0
		client.print("<li>Average score: ");
		client.print(user_settings.total_points / user_settings.games_played);
		client.println("</li>");
	}
	client.print("<li>High score: ");
	client.print(user_settings.high_score);
	client.println("</li>");
	client.print("<li>Boss kills: ");
	client.print(user_settings.boss_kills);
	client.println("</li>");
	client.println("<p></p>"); // add some whitespace
	client.print("<li>Current level: ");
	client.print(levelNumber);
	client.println("</li>");
	client.print("<li>Current score: ");
	client.print(score);
	client.println("</li>");
	client.println("</ul>");

	client.print("<button onClick=\"location.href = 'http://192.168.4.1'\">Refresh</button>");

	client.print("<h2>Adjustable Settings</h2>");
	client.print("<table>");
	
	client.print("<tr>");
	client.printf("<td>LED Count (%d-%d)</td>", MIN_LEDS, MAX_LEDS);
	client.printf("<td><form><input type='number' name='E' value='%d' min='%d' max='%d'><input type='submit'></form></td>",
			user_settings.led_end, MIN_LEDS, MAX_LEDS);
	client.print("</tr>");
	
	client.print("<tr>");
	client.printf("<td>LED Offset (%d-%d)</td>", 0, user_settings.led_end-MIN_LEDS);
	client.printf("<td><form><input type='number' name='O' value='%d' min='%d' max='%d'><input type='submit'></form></td>",
			user_settings.led_offset, 0, user_settings.led_end-MIN_LEDS);
	client.print("</tr>");

	client.print("<tr><td>Brightness (10-255)</td><td><form><input type='number' name='B' value='");
	client.print(user_settings.led_brightness);
	client.print("' min='10' max='255'><input type='submit'></form></td></tr>");

	client.print("<tr><td>Screensaver brightness (10-255)</td><td><form><input type='number' name='C' value='");
	client.print(user_settings.led_brightnessScreensaver);
	client.print("' min='10' max='255'><input type='submit'></form></td></tr>");

	client.printf("<tr><td>Strip mode (%d-%d)<br><small>0 = RGB, 1 = RGBW, 2 = RGBW with white channel off<br>currently: %s</small></td>",
			MIN_STRIP_MODE, MAX_STRIP_MODE, strip_mode_name(user_settings.strip_mode));
	client.printf("<td><form><input type='number' name='W' value='%d' min='%d' max='%d'><input type='submit'></form></td></tr>",
			user_settings.strip_mode, MIN_STRIP_MODE, MAX_STRIP_MODE);

	client.print("<tr><td>Sound Volume (0-255)</td><td><form><input type='number' name='S' value='");
	client.print(user_settings.audio_volume);
	client.print("' min='0' max='255'><input type='submit'></form></td></tr>");

	client.print("<tr><td>Joystick Deadzone (3-12)</td><td><form><input type='number' name='D' value='");
	client.print(user_settings.joystick_deadzone);
	client.print("' min='3' max='12'><input type='submit'></form></td></tr>");

	client.print("<tr><td>Attack Sensitivity (20000-30000)</td><td><form><input type='number' name='A' value='");
	client.print(user_settings.attack_threshold);
	client.print("' min='20000' max='30000'><input type='submit'></form></td></tr>");

	client.print("<tr><td>Lives Per Level (1-9)</td><td><form><input type='number' name='L' value='");
	client.print(user_settings.lives_per_level);
	client.print("' min='1' max='9'><input type='submit'></form></td></tr>");

	client.print("<tr><td>Skip to level (0-20)</td><td><form><input type='number' name='V' value='");
	client.print(levelNumber);
	client.print("' min='0' max='20'><input type='submit'></form></td></tr>");

	client.print("</table>");

#ifdef ENABLE_PROMETHEUS_METRICS_ENDPOINT
	client.println("<ul><li><a href=\"/metrics\">Metrics</a></li></ul>");
#endif // ENABLE_PROMETHEUS_METRICS_ENDPOINT

	client.println("</body>");
	client.println("</html>");
	client.println();
}

#ifdef ENABLE_PROMETHEUS_METRICS_ENDPOINT
// We need to use print() here since println() prints newlines as CR/LF, which
// Prometheus cannot handle.
#define __prom_metric(metric_name, metric_description, value)        \
	client.print("# HELP " metric_name " " metric_description "\n"); \
	client.print("# TYPE " metric_name " gauge\n");                  \
	client.print(metric_name " ");                                   \
	client.print(value);                                             \
	client.print("\n");

static void sendMetricsPage(WiFiClient client)
{
	client.println("HTTP/1.1 200 OK");
	client.println("Content-Type: text/plain; charset=utf-8");
	client.println("Server: twang_exporter");
	client.println();
	__prom_metric("twang_games_played", "Number of games played", user_settings.games_played);
	__prom_metric("twang_total_points", "Total points", user_settings.total_points);
	__prom_metric("twang_high_score", "High score", user_settings.high_score);
	__prom_metric("twang_boss_kills", "Boss kills", user_settings.boss_kills);
}

#undef __prom_metric
#endif // ENABLE_PROMETHEUS_METRICS_ENDPOINT

settings_param_t ap_client_check()
{
	int cnt;
	bool newconn = false;
	int stat;
	WiFiClient client = server.available(); // listen for incoming clients

	bool currentLineIsBlank = true;
	PAGE_TO_SEND page_to_send = Stats;

	settings_param_t param = SET_PARAM_INVALID;

	while (client.connected())
	{
		if (client.available())
		{
			char c = client.read();
			linebuf[charcount] = c;
			if (charcount < sizeof(linebuf) - 1)
				charcount++;

			if (c == '\n' && currentLineIsBlank)
			{
				switch (page_to_send)
				{
				case Stats:
					sendStatsPage(client);
					break;
#ifdef ENABLE_PROMETHEUS_METRICS_ENDPOINT
				case Metrics:
					sendMetricsPage(client);
					break;
#endif // ENABLE_PROMETHEUS_METRICS_ENDPOINT
				}
				break;
			}
			if (c == '\n')
			{
				if (strstr(linebuf, "GET /?") > 0)
				{
					String line = String(linebuf);

					int start = line.indexOf('=', 0);

					char paramCode = line.charAt(start - 1);

					int finish = line.indexOf('H', start + 1) - 1;
					String val = line.substring(start + 1, finish);
					// if it is not numeric, it will convert to 0.
					// The the change_setting function will make sure the range is OK

					param = settings_param_t{.code = paramCode, .hasValue = true, .newValue = (uint16_t)val.toInt()};

					page_to_send = Stats;
				}
#ifdef ENABLE_PROMETHEUS_METRICS_ENDPOINT
				else if (strstr(linebuf, "GET /metrics"))
				{
					page_to_send = Metrics;
				}
#endif // ENABLE_PROMETHEUS_METRICS_ENDPOINT

				// you're starting a new line
				currentLineIsBlank = true;
				memset(linebuf, 0, sizeof(linebuf));
				charcount = 0;
			}
			else if (c != '\r')
			{
				// you've gotten a character on the current line
				currentLineIsBlank = false;
			}
		}
	}

	return param;
}
