<?php

// TODO: MODIFY THESE CUSTOM VALUES
$FROM_PHONE_NUMBER = '+15125551212';
$TWILIO_SID = 'YOUR-TWILIO-SID-GOES-HERE';
$TWILIO_TOKEN = 'YOUR-TWIIO-TOKEN-GOES-HERE';
// END CUSTOM VALUES

$debug = true;
$dbg_msgs = array();

require_once 'Twilio/autoload.php';
// Use the REST API Client to make requests to the Twilio REST API
use Twilio\Rest\Client;

function send_sms($to, $msg)
{
  global $FROM_PHONE_NUMBER;
  global $TWILIO_SID;
  global $TWILIO_TOKEN;

  $client = new Client($TWILIO_SID, $TWILIO_TOKEN);

  // Use the client to do fun stuff like send text messages!
  $client->messages->create(
    // the number you'd like to send the message to
    $to,
    [
      // A Twilio phone number you purchased at twilio.com/console
      'from' => $FROM_PHONE_NUMBER,
      // the body of the text message you'd like to send
      'body' => $msg
    ]
  );
}

function pb($b)
{
    if ($b) {
      return 'true';
    } else {
      return 'false';
    }
}

function dbg($msg)
{
    global $debug;
    global $dbg_msgs;
    if ($debug) {
        $dbg_msgs[] = htmlentities($msg);
    }
}

function err_page($msg)
{
    global $debug;
    global $dbg_msgs;
    header("HTTP/1.1 {$msg}");
?><!DOCTYPE html>
<html>
 <head>
   <title><?php echo $msg; ?></title>
 </head>
 <body>
  <h1><?php echo $msg; ?></h1>
<?php
    if (count($dbg_msgs) > 0)
    {
        echo "  <ul>\n";
        foreach ($dbg_msgs as $m)
        {
            echo "    <li>{$m}</li>\n";
        }
        echo "  </ul>\n";
    }
?>
 </body>
</html>
<?php
    exit();
}

function msg_page($title, $body)
{
    global $debug;
    global $dbg_msgs;
    header("HTTP/1.1 200 ok");
?><!DOCTYPE html>
<html>
 <head>
   <title><?php echo $title; ?></title>
 </head>
 <body><div>
<?php echo $body; ?></div>
<?php
    if (count($dbg_msgs) > 0)
    {
        echo "  <ul>\n";
        foreach ($dbg_msgs as $m)
        {
            echo "    <li>{$m}</li>\n";
        }
        echo "  </ul>\n";
    }
?>
 </body>
</html>
<?php
    exit();
}

function not_found()
{
    err_page('404 Not Found');
}

function data_dir()
{
    $cwd = realpath(dirname(__FILE__));
    $ddir = $cwd . "/data/" . $_SERVER["SERVER_NAME"];
    return $ddir;
}

function humanTime($ts)
{
    // 2021-Oct-12 02:05:00
    return date('Y-M-d H:i:s', $ts);
}

function show_summary($db)
{
    $d = "";
    $ts = time();
    $results = $db->query("SELECT * FROM watchdogs");
    while ($row = $results->fetchArray())
    {
        if ($row["wdt_last_timestamp"] == 0)
        {
            $d .= "<div>{$row['wdt_name']} expired</div>";
        }
        else if ($ts > ($row["wdt_last_timestamp"] + $row["wdt_frequency"]))
        {
            // send a text on watchdogs
            send_sms($row['wdt_sms_number'], $row['wdt_timeout_msg']);
            $db->exec("UPDATE watchdogs SET wdt_last_timestamp=0 WHERE wdt_name=\"{$row["wdt_name"]}\"");
            $d .= "<div>{$row['wdt_name']} fired</div>";
        }
        else
        {
            $time_since_last = $ts - $row['wdt_last_timestamp'];
            $name = preg_replace('/[-_]/', ' ', $row['wdt_name']);
            $d .= "<div>{$name} OK ({$time_since_last}/{$row['wdt_frequency']})</div>";
        }
    }
    if ($d == "") {
        $d = "Nothing to report. Now fuck off.";
    }
     msg_page("Uptime Status", $d);
}


function log_data()
{
    $f = data_dir() . "/uptime.log";
    if (false) {
    // rotate files as needed
    // keep 24-48 hours in current file; truncate, splitting on day boundaries
    $first_current = new DateTime();
    $first_current->setTime(0,0);
    $first_current->sub(DateInterval::createFromDateString('1 day'));
    $ts = $first_current->getTimestamp();
    }
    // remove /uptime/log/ from beginning
    $msg = preg_replace(',/uptime/log[/?]*,', '', $_SERVER['REQUEST_URI']);
    $msg = urldecode($msg);
    $t = time();
    file_put_contents($f, "${t}: {$msg}\n", FILE_APPEND);
}

function mean($a)
{
  return array_sum($a) / (1.0 * sizeof($a));
}

function median($a)
{
  sort($a);
  $n = sizeof($a);
  if (!$n) {
    return 0;
  }
  $mid = floor($n / 2);
  if ($n & 1) {
    return $a[$mid];
  }
  return ($a[$mid - 1] + $a[$mid]) / 2.0;
}

function stddev($a)
{
  $sum = 0;
  $m = mean($a);
  foreach ($a as $s)
  {
    $v = $s - $m;
    $sum += $v * $v;
  }
  $sum /= (sizeof($a) - 1);
  return sqrt($sum);
}

class Ave
{
  public $win_size;
  public $count;
  public $sum;
  function __construct($win_size)
  {
    $this->win_size = $win_size;
    $this->count = 0;
    $this->value = 0;
  }
  function slide($val)
  {
    $prev_count = ($this->count == $this->win_size) ? ($this->win_size - 1) : $this->count;
    $this->count = $prev_count + 1;
    $this->value = ($prev_count * $this->value + $val) / $this->count;
  }
}

function tail($fname, $n=1000)
{
  $fs = filesize($fname);
  $seek = 0;
  if ($fs > $n)
  {
    $seek = $fs - $n;
  }
  return file_get_contents($fname, false, null, $seek);
}

require_once 'jpgraph/jpgraph.php';
require_once 'jpgraph/jpgraph_line.php';
require_once 'jpgraph/jpgraph_date.php';

// special case for wh-usage (interpreting data, not just plotting)
function wh_usage($d)
{
  $q = 'flame_v_ave';
  // divide the samples into equal parts, taking averages
  $log = data_dir() . "/uptime.log";
  $lines = file_get_contents($log);
  $lines = explode("\n", $lines);
  $ydata = array(array(), array());
  $yadata = array(array(), array());
  $qlen = strlen($q);
  $min = 999999999999;
  $max = 0;
  $first = time() - $d * 86400;
  $utc = new DateTime('now', new DateTimeZone('UTC'));
  $pdt = new DateTime('now', new DateTimeZone('America/Los_Angeles'));
  $delta_t = $pdt->getOffset() - $utc->getOffset();
  foreach ($lines as $line)
  {
    if (strpos($line, $q)) {
      $parts = preg_split('/[&,;:\s]+/', $line, -1, PREG_SPLIT_NO_EMPTY);
      $ts = intval($parts[0]);
      if ($ts < $first)
      {
        continue;
      } 
      foreach ($parts as $p)
      {
        if (preg_match('/([_.a-z0-9]+)=([_.a-z0-9]+)/', $p, $kv) == 1) {
          if ($kv[1] == $q) {
            $v = floatval(substr($p, $qlen+1));
            if ($v < $min)
            {
              $min = $v;
            }
            if ($v > $max)
            {
              $max = $v;
            }
            $ydata[0][] = $ts + $delta_t;
            $ydata[1][] = $v;
          }
        }
      }
    }
  }
  // go through ydata and look for bumps
  // ave values above 10 are 'on'
  $total_time_on = 0;
  $wh_data = array(array(), array());
  $on = false;
  for ($i=0; $i<count($ydata[0]); $i++)
  {
    if ($ydata[1][$i] > 10.5)
    {
      if (!$on)
      {
        $on_ts = $ydata[0][$i];
        $on = true;
      }
    }
    // $ydata[1][$i] <= 10.5
    else
    {
      if ($on)
      {
        $on = false;
        $off_ts = $ydata[0][$i];
        $on_time = ($off_ts - $on_ts) / 60;
        $total_time_on += $on_time;
        $wh_data[0][] = $on_ts - 120;
        $wh_data[1][] = 0;
        $wh_data[0][] = $on_ts;
        $wh_data[1][] = $on_time;
        $wh_data[0][] = $off_ts;
        $wh_data[1][] = $on_time;
        $wh_data[0][] = $off_ts + 120;
        $wh_data[1][] = 0;
      }
    }
  }
  if ($on)
  {
    $wh_data[0][] = $on_ts;
    $on_time = ($ydata[0][-1] + 120 - $on_ts) / 60;
    $wh_data[1][] = $on_time;
    $total_time_on += $on_time;
  }
 
  $debug_data = false;
  if ($debug_data)
  {
    header('Content-type: text/plain');
    print_r($ydata);
    print_r($wh_data);
    return;
  }

  // only plot if there is data to plot
  if (count($ydata[1]) == 0)
  {
    not_found();
    return;
  }
  // Width and height of the graph
  $width = 1600; $height = 600;

  // Create a graph instance
  $graph = new Graph($width, $height);

  // Specify what scale we want to use,
  // int = integer scale for the X-axis
  // int = integer scale for the Y-axis
  $graph->SetScale('datlin');

  $therms = (($total_time_on / 60) * 65000) / 100000;
  // Setup a title for the graph
  $t = sprintf("WH usage over time: last %d days\n%0.2f minutes (%0.3f therms)",
    $d, $total_time_on, $therms);
  $graph->title->Set($t);

  // Setup titles and X-axis labels
  $graph->xaxis->title->Set('time');
  $graph->xaxis->SetLabelAngle(60);
  $graph->xaxis->scale->SetTimeAlign(MINADJ_30);

  $graph->yaxis->HideLine(false);
  $graph->yaxis->HideTicks(false,false);

  $graph->xgrid->Show();
  $graph->xgrid->SetLineStyle("solid");

  // Setup Y-axis title
  $graph->yaxis->title->Set("$q");

  // Create the linear plot
  $yplot = new LinePlot($ydata[1], $ydata[0]);
  $yplot->SetColor("grey");

  // Add the plot to the graph
  $graph->Add($yplot);
  // lines for averages
  $yaplot = new LinePlot($wh_data[1], $wh_data[0]);
  $yaplot->SetColor("black");
  // Add the plot to the graph
  $graph->Add($yaplot);

  // Display the graph
  $graph->Stroke();
}

function plot_data($q, $d)
{
  if ($q == 'usage')
  {
    return wh_usage($d);
  }
  $q_ave = "{$q}_ave";
  // divide the samples into equal parts, taking averages
  $log = data_dir() . "/uptime.log";
  $lines = file_get_contents($log);
  $lines = explode("\n", $lines);
  $xdata = array();
  $ydata = array(array(), array());
  $yadata = array(array(), array());
  $qlen = strlen($q);
  $q_avelen = strlen($q_ave);
  $min = 999999999999;
  $max = 0;
  $first = time() - $d * 86400;
  $last_ts = 0;
  $last_tick = 0;
  $utc = new DateTime('now', new DateTimeZone('UTC'));
  $pdt = new DateTime('now', new DateTimeZone('America/Los_Angeles'));
  $delta_t = $pdt->getOffset() - $utc->getOffset();
  $ave = new Ave(10);
  foreach ($lines as $line)
  {
    if (strpos($line, $q)) {
      $parts = preg_split('/[&,;:\s]+/', $line, -1, PREG_SPLIT_NO_EMPTY);
      $ts = intval($parts[0]);
      $tick = 0;
      if ($ts < $first)
      {
        continue;
      } 
      foreach ($parts as $p)
      {
        if (preg_match('/([_.a-z0-9]+)=([_.a-z0-9]+)/', $p, $kv) == 1) {
          if ($kv[1] == "t")
          {
            $tick = intval(substr($p, 2));
            if ($last_ts)
            {
              $tick_ave = $ave->slide(($ts - $last_ts)/($tick - $last_tick));
            }
          }
          else if ($kv[1] == $q) {
            $v = floatval(substr($p, $qlen+1));
            if ($v < $min)
            {
              $min = $v;
            }
            if ($v > $max)
            {
              $max = $v;
            }
            $ydata[0][] = $ts + $delta_t;
            $ydata[1][] = $v;
          }
          else if ($kv[1] == $q_ave) {
            $v = floatval(substr($p, $q_avelen+1));
            $yadata[0][] = $ts + $delta_t;
            $yadata[1][] = $v;
          }
        }
      }
      if ($tick) {
        $last_ts = $ts;
        $last_tick = $tick;
      }
    }
  }
  $uptime = $last_tick * $ave->value;
  if ($uptime > 86400)
  {
    $uptime = round($uptime / 86400);
    $uptime = "{$uptime}d";
  }
  else if ($uptime > 1.5 * 3600)
  {
    $uptime = round($uptime / 3600);
    $uptime = "{$uptime}h";
  }
  else if ($uptime > 1.5 * 60)
  {
    $uptime = round($uptime / 60);
    $uptime = "{$uptime}m";
  }
  $scaled = false;
  $sd = stddev($ydata[1]);
  $m = median($ydata[1]);
  if ($sd > $m)
  {
    foreach ($ydata[1] as $k => $v)
    {
      if (abs($v - $m) > $m)
      {
        $sign = sign($v - $m);
        $ydata[1][$k] = $m + $sign * 20 * log(abs($v - $m));
        $scaled = true;
      }
    }
  }
 
  $debug_data = false;
  if ($debug_data)
  {
    header('Content-type: text/plain');
    print_r($ydata);
    print_r($yadata);
    return;
  }

  // only plot if there is data to plot
  if (count($ydata[1]) == 0)
  {
    not_found();
    return;
  }
  // Width and height of the graph
  $width = 1600; $height = 600;

  // Create a graph instance
  $graph = new Graph($width, $height);

  // Specify what scale we want to use,
  // int = integer scale for the X-axis
  // int = integer scale for the Y-axis
  $graph->SetScale('datlin');

  // Setup a title for the graph
  $scaled = $scaled ? "scaled, " : "";
  $graph->title->Set("{$q} over time; [{$min}..{$max}]\nUptime: {$uptime}\n{$scaled}median: {$m}, stddev: {$sd}");

  // Setup titles and X-axis labels
  $graph->xaxis->title->Set('time');
  $graph->xaxis->SetLabelAngle(60);
  $graph->xaxis->scale->SetTimeAlign(MINADJ_30);

  $graph->yaxis->HideLine(false);
  $graph->yaxis->HideTicks(false,false);

  $graph->xgrid->Show();
  $graph->xgrid->SetLineStyle("solid");

  // Setup Y-axis title
  $graph->yaxis->title->Set("$q");

  // Create the linear plot
  $yplot = new LinePlot($ydata[1], $ydata[0]);
  if (count($yadata[0]) > 0) {
    $yplot->SetColor("grey");
  } else {
    $yplot->SetColor("black");
  }

  // Add the plot to the graph
  $graph->Add($yplot);
  if (count($yadata[0]) > 0)
  {
    // lines for averages
    $yaplot = new LinePlot($yadata[1], $yadata[0]);
    $yaplot->SetColor("black");
    // Add the plot to the graph
    $graph->Add($yaplot);
  }

  // Display the graph
  $graph->Stroke();
}

function get_plot_vars()
{
  // divide the samples into equal parts, taking averages
  $log = data_dir() . "/uptime.log";
  $lines = tail($log);
  $lines = explode("\n", $lines);
  for ($n = sizeof($lines) - 1; $n >= 0; $n--)
  {
    $line = $lines[$n];
    if (strstr($line, ": t="))
    {
      break;
    }
  }
  $names = array('usage');
  $out = array();
  if (!preg_match_all('/([_a-z0-9]+)=([_a-z0-9]+)/', $line, $allm))
  {
    return array();
  }
  foreach ($allm[1] as $m)
  {
    if ((strlen($m) == 1) || (substr($m, -4) == "_ave"))
    {
      continue;
    }
    $names[] = $m;
  }

  return $names;
}

function plot_all_data($d)
{
    header("HTTP/1.1 200 ok");
?><!DOCTYPE html>
<html>
 <head>
   <title>Uptime Plots</title>
 </head>
 <body>
<?php
    $vars = get_plot_vars();
    foreach ($vars as $v)
    {
      $p = "/uptime/plot/$v";
      if ($d != 1)
      {
        $p .= "?d={$d}";
      }
      echo "<div><h2>$v</h2><div><a href=\"{$p}\"><img src=\"{$p}\" alt=\"$v\"/></a></div></div>\n";
    }
?>
 </body>
</html>
<?php
    exit();
}

function log_uptime($db, $q)
{
     $ts = time();
     $db->exec("UPDATE watchdogs SET wdt_last_timestamp=$ts WHERE wdt_name=\"$q\"");
     msg_page("Uptime", "Thank you for reporting $q");
}

function process_query($db) {
    if (!isset($_REQUEST['q'])) {
      dbg("at: " . __LINE__);
        not_found();
        return;
    }
    $d = 1;
    if (isset($_REQUEST['d']))
    {
      $d = intval($_REQUEST['d']);
    }
    $q = $_REQUEST['q'];
    if ($q == '' || $q == '/')
    {
        show_summary($db);
    }
    else if (substr($q, 0, 4) == "log/" || $q == "log")
    {
      log_data();
    }
    else if ($q == "plots" || $q == "plots/")
    {
      plot_all_data($d);
    }
    else if (substr($q, 0, 5) == "plot/")
    {
      $v = substr($q, 5);
      plot_data($v, $d);
    }
    else
    {
        log_uptime($db, $q);
    }
}

function init_db()
{
    $db = new SQLite3(data_dir() . "/uptime.sqlite3");
    $init_tables = "CREATE TABLE IF NOT EXISTS watchdogs (
                        wdt_id   INTEGER PRIMARY KEY,
                        wdt_name TEXT NOT NULL,
                        wdt_frequency INTEGER NOT NULL,
                        wdt_last_timestamp INTEGER DEFAULT 0,
                        wdt_sms_number TEXT NOT NULL,
                        wdt_timeout_msg TEXT NOT NULL
                      )";
    $db->exec($init_tables);
    return $db;
}

header("Cache-Control: no-cache, must-revalidate"); // HTTP/1.1
header("Expires: Sat, 26 Jul 1997 05:00:00 GMT"); // Date in the past

$db = init_db();
process_query($db);
$db->close();
